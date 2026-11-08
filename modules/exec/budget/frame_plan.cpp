#include "db/exec/frame_plan.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "db/common/error.h"

namespace db::exec
{
    namespace
    {
        // How an operator uses frames. Ops with the same phases share a kind.
        //In database engines, operators are either streaming (process data row-by-row on the fly)
        //or blocking (must read and buffer the entire input before producing results).
        enum class Kind
        {
            // seqscan: holds its one data page while streaming.
            Scan,
            // filter, project, limit, rename: hold one tuple, pass the
            // all available frame allocation down to its child operator.
            PassThrough,
            // union_all: like PassThrough, but in sequence streams the left child,
            // closes it, then opens and streams the right child.
            UnionAll,
            // sort; distinct and group_by in sort mode. Blocking phase Open: pass 0
            // (Reads all child data into memory frames and sorts them into runs),
            // Streaming phase (Stream): Merges the sorted runs and streams the final sorted output.
            SortOne,
            // distinct and group_by in hash mode. Blocking phase Open: build or partition
            // while the child streams. Stream: one partition at a time (a table,
            // or a sort if the partition does not fit in the table).
            HashOne,
            // sort_merge_join, union, intersect, except. Blocking phase Open: sort the left
            // input like SortOne, then the right one like SortOne. Stream phase: merge the two
            // sorted runs. union (chunk 08 Q1 (a)) instead feeds both inputs, one
            // after the other, into ONE sorter and streams its final merge; the
            // frame numbers are the same (run size, merges, 2 for streaming).
            SortTwo,
            // hash_join. Open: partition the left input, then the right one,
            // into the same P partition pages. Stream: probe partition by partition.
            HashTwo,
            // nested_loop_join. Stream: the outer streams into the block,
            // and the inner is opened and streamed again for every block.
            NestedLoop
        };

        Kind kindOf(const PlanNode& node)
        {
            switch (node.op)
            {
            case PlanOp::SeqScan:
                return Kind::Scan;
            case PlanOp::Filter:
            case PlanOp::Project:
            case PlanOp::Limit:
            case PlanOp::Rename:
                return Kind::PassThrough;
            case PlanOp::UnionAll:
                return Kind::UnionAll;
            case PlanOp::Sort:
                return Kind::SortOne;
            case PlanOp::Distinct:
            case PlanOp::GroupBy:
                // The plan reader requires `mode` on these (DECISIONS 2026-11-01).
                return *node.mode == ExecMode::Sort ? Kind::SortOne : Kind::HashOne;
            case PlanOp::SortMergeJoin:
            case PlanOp::Union:
            case PlanOp::Intersect:
            case PlanOp::Except:
                return Kind::SortTwo;
            case PlanOp::HashJoin:
                return Kind::HashTwo;
            case PlanOp::NestedLoopJoin:
                return Kind::NestedLoop;
            case PlanOp::Scan:
            case PlanOp::Select:
            case PlanOp::Join:
            case PlanOp::Cross:
                break;
            }
            // Logical-only ops never reach exec: physicalFromJson rejects them.
            throw std::logic_error("divideFrames: '" + std::string(opName(node.op)) + "' is not a physical op");
        }

        // The fewest frames a node's subtree needs, in each half of its life.
        struct Need
        {
            // During open(): the node's own blocking work plus the children
            // it opens.
            std::size_t open = 0;
            // During next(): the node's streaming work plus the children it
            // pulls from.
            std::size_t stream = 0;
            // Nodes in the subtree, the node included. Used to find the
            // right child's pre-order position.
            std::size_t subtreeSize = 0;
        };

        // The fewest frames for the whole life of a subtree.
        std::size_t total(const Need& need)
        {
            return std::max(need.open, need.stream);
        }

        // The first node (children before parents) whose subtree needs
        // more than B. Because children come first, all of its children fit,
        // so it is the node to blame in the error message.
        // Stores error metadata if a query exceeds physical frame limits.
        struct TooBig
        {
            bool found = false;
            PlanOp op = PlanOp::SeqScan;
            std::size_t need = 0;
        };

        /// why we need 2 passes and 2 different order
        /// Bottom-Up (Post-Order): Needed to calculate requirements. A parent cannot determine its
        /// own memory needs until its children have calculated theirs.
        /// Top-Down (Pre-Order): Needed to distribute the actual frame budget.
        /// Memory allocation starts at the root (the total physical memory budget B)
        /// and flows downward to the leaves.


        // The stream need of a node that can write an input to a run
        // (nested_loop_join, union_all): the need without writing if it still
        // leaves at least 1 frame of B for a parent, else the need with the
        // input written. A blocking parent gives its child exactly this
        // minimum, so using the written form when it is not needed would make
        // the child write its input at every B. Every parent needs at most 1
        // frame beside a streaming child, or can write that child itself, so
        // this never makes a plan miss B.
        std::size_t writeOnlyIfNeeded(std::size_t withoutWriting, std::size_t withWriting, std::size_t frames)
        {
            return withoutWriting + 1 <= frames ? withoutWriting : withWriting;
        }

        // Pass 1, Bottom-Up Analysis (Post-Order): needs[i] for every node of the subtree, where i
        // is the node's pre-order position. Children are finished before
        // the node, so their needs are known when the node's are computed.
        void computeNeeds(const PlanNode& node, std::size_t frames, std::vector<Need>& needs, TooBig& tooBig)
        {
            const std::size_t index = needs.size();
            // creates a default-initialized instance of the Need struct and
            // appends (pushes) it to the end of the needs vector.
            needs.push_back({});
            // Positions, not references: push_back may move the vector.
            std::size_t left = 0;
            std::size_t right = 0;
            if (node.children.size() >= 1)
            {
                left = needs.size();
                computeNeeds(node.children[0], frames, needs, tooBig);
            }
            if (node.children.size() >= 2)
            {
                right = needs.size();
                computeNeeds(node.children[1], frames, needs, tooBig);
            }

            Need need;
            switch (kindOf(node))
            {
            case Kind::Scan:
                need.stream = 1; // one data page pinned at a time
                break;
            case Kind::PassThrough:
                need.open = needs[left].open;
                need.stream = needs[left].stream;
                break;
            case Kind::UnionAll:
                // The right child is opened only after the left one is
                // finished, during next(), so its whole need falls there. If
                // that is more than the stream share, the right child is
                // written to a run in open() instead (running alone: its open
                // need, or its stream need + 1 writer page) and read with 1
                // frame (FrameShare::writeInner). Writing is only planned when
                // the unwritten form would leave no frame for a parent at this
                // B (see writeOnlyIfNeeded).
                need.open = std::max({needs[left].open, needs[right].open, 1 + needs[right].stream});
                need.stream = writeOnlyIfNeeded(std::max(needs[left].stream, total(needs[right])),
                                                needs[left].stream, frames);
                break;
            case Kind::SortOne:
                // Pass 0: a run of at least 1 page + the streaming child.
                // Merge passes: 2 input runs + 1 output page = 3.
                // Opening the child happens while the sort holds nothing.
                // Final merge: at least 2 (runs read side by side).
                need.open = std::max({1 + needs[left].stream, std::size_t{3}, needs[left].open});
                need.stream = 2;
                break;
            case Kind::HashOne:
                // Build or partition: at least 1 page + the streaming child.
                // Per partition: at least 1 table page + 1 reader page; but a
                // partition with too many different tuples is sorted instead
                // (chunk 09 Q1 (a)), and a sort's merge needs 3.
                need.open = std::max(1 + needs[left].stream, needs[left].open);
                need.stream = 3;
                break;
            case Kind::SortTwo:
                // Each input is sorted like SortOne, one after the other.
                // Merge: one reader page per sorted input = 2.
                need.open = std::max({
                    1 + std::max(needs[left].stream, needs[right].stream), std::size_t{3},
                    needs[left].open, needs[right].open
                });
                // Merge: both sorts stream their final merge side by side, at
                // least 1 run each = 2. sort_merge_join also keeps the current
                // run of equal right keys in 1 page (it spills to a temp run
                // when longer): 3, the spec's "3 for the merge" (§M2.4).
                need.stream = node.op == PlanOp::SortMergeJoin ? 3 : 2;
                break;
            case Kind::HashTwo:
                // Partition each input in turn: at least 1 partition page +
                // that streaming input. Probe: 1 table page + 1 reader page;
                // but a partition too big for its table is joined by block
                // nested loop (chunk 13 Q1 (b)): a block of at least 1 page +
                // a reader on each partition = 3.
                need.open = std::max({
                    1 + std::max(needs[left].stream, needs[right].stream), needs[left].open,
                    needs[right].open
                });
                need.stream = 3;
                break;
            case Kind::NestedLoop:
                // A block of at least 1 page + 1 frame for each input. An
                // input that needs more is written to a temporary run first
                // (FrameShare::writeOuter/writeInner), and then read with 1
                // frame. Writing one happens in open(), with the input running
                // alone: its open need, or its stream need + 1 writer page.
                need.open = std::max({
                    needs[left].open, 1 + needs[left].stream, needs[right].open, 1 + needs[right].stream
                });
                need.stream = writeOnlyIfNeeded(1 + needs[left].stream + total(needs[right]), 3, frames);
                break;
            }
            need.subtreeSize = needs.size() - index;
            needs[index] = need;

            if (!tooBig.found && total(need) > frames)
                tooBig = {true, node.op, total(need)};
        }

        // Pass 2,Top-Down Distribution (Pre-Order): the node at pre-order position `index` may use
        // `open` frames inside open() and `stream` frames during next().
        // Pass 1 made sure both are at least its needs. In each phase it
        // gives each running child exactly that child's need and keeps the
        // rest; a child it opens while holding nothing gets all of `open`.
        void assignShares(const PlanNode& node, std::size_t index, std::size_t open, std::size_t stream,
                          const std::vector<Need>& needs, std::vector<FrameShare>& shares)
        {
            // Pre-order: the left child comes right after the node, the
            // right child right after the left child's subtree.
            const std::size_t left = index + 1;
            const std::size_t right = node.children.size() >= 2 ? left + needs[left].subtreeSize : 0;

            switch (kindOf(node))
            {
            case Kind::Scan:
                shares[index] = {open, stream, 1};
                break;
            case Kind::PassThrough:
                shares[index] = {open, stream, 0};
                assignShares(node.children[0], left, open, stream, needs, shares);
                break;
            case Kind::UnionAll:
                {
                    const bool writeRight = total(needs[right]) > stream;
                    shares[index] = {open, stream, 0, false, writeRight};
                    assignShares(node.children[0], left, open, stream, needs, shares);
                    if (writeRight)
                        assignShares(node.children[1], right, open, open - 1, needs, shares);
                    else // opened during next(), so both of its halves come out of `stream`
                        assignShares(node.children[1], right, stream, stream, needs, shares);
                    break;
                }
            case Kind::SortOne:
            case Kind::HashOne:
                // Own frames while the child streams: pass 0's run, or the
                // hash table / partition pages.
                shares[index] = {open, stream, open - needs[left].stream};
                assignShares(node.children[0], left, open, needs[left].stream, needs, shares);
                break;
            case Kind::SortTwo:
            case Kind::HashTwo:
                {
                    // One number for both inputs: the run size (SortTwo) or P
                    // (HashTwo) leaves room for the larger streaming input.
                    const std::size_t inputs = std::max(needs[left].stream, needs[right].stream);
                    shares[index] = {open, stream, open - inputs};
                    assignShares(node.children[0], left, open, needs[left].stream, needs, shares);
                    assignShares(node.children[1], right, open, needs[right].stream, needs, shares);
                    break;
                }
            case Kind::NestedLoop:
                {
                    // The block is what is left after the streaming outer and
                    // the whole inner (reopened while the block is held):
                    // B - 2 when both are seqscans (§M2.3). If they do not fit
                    // beside a block of 1 page, the inner is written to a run
                    // first (it would be redone for every block otherwise),
                    // then the outer if still needed; each is then read with
                    // 1 frame. At a large B nothing is written.
                    const std::size_t outerNeed = needs[left].stream;
                    const std::size_t innerNeed = total(needs[right]);
                    const bool writeInner = 1 + outerNeed + innerNeed > stream && innerNeed > 1;
                    const std::size_t innerFrames = writeInner ? 1 : innerNeed;
                    const bool writeOuter = 1 + outerNeed + innerFrames > stream && outerNeed > 1;
                    const std::size_t outerFrames = writeOuter ? 1 : outerNeed;
                    shares[index] = {open, stream, stream - outerFrames - innerFrames, writeOuter, writeInner};
                    // A written input runs alone inside open(), streaming into
                    // one writer page; otherwise it gets exactly its need.
                    if (writeOuter)
                        assignShares(node.children[0], left, open, open - 1, needs, shares);
                    else
                        assignShares(node.children[0], left, open, outerNeed, needs, shares);
                    if (writeInner)
                        assignShares(node.children[1], right, open, open - 1, needs, shares);
                    else
                        assignShares(node.children[1], right, innerNeed, innerNeed, needs, shares);
                    break;
                }
            }
        }
    } // namespace

    FramePlan divideFrames(const PlanNode& plan, std::size_t frames)
    {
        std::vector<Need> needs;
        TooBig tooBig;
        computeNeeds(plan, frames, needs, tooBig);

        FramePlan result;
        result.minimumFrames = total(needs[0]);
        if (tooBig.found)
            throw Error(ErrorCategory::Schema,
                        "Node '" + std::string(opName(tooBig.op)) + "' needs at least " + std::to_string(tooBig.need)
                        + " frames; the plan needs at least " + std::to_string(result.minimumFrames) + ", but B is "
                        + std::to_string(frames) + ".");

        // resize vector to specific size supplied
        result.shares.resize(needs.size());
        assignShares(plan, 0, frames, frames, needs, result.shares);
        return result;
    }
} // namespace db::exec
