#pragma once
/// @file
/// Block nested loop join (SPEC §M2.3 nested_loop_join, chunk 11).

#include <cstddef>
#include <memory>

#include "db/common/condition.h"
#include "db/common/evaluator.h"
#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"
#include "db/exec/tuple_buffer.h"

namespace db::exec
{
    /// Reads the left (outer) input in blocks and streams the whole right
    /// (inner) input past each block, testing every pair:
    ///
    ///   for each block of the outer (share.ownFrames pages, B - 2 for two scans)
    ///       open the inner, and for each inner tuple s
    ///           for each tuple r in the block: test (r, s), output r + s if it holds
    ///       close the inner
    ///
    /// The inner is read once per block: cost M + ceil(M / (B-2)) * N page
    /// reads. Every pair is one comparison, matching or not, so comparisons
    /// = |R| * |S| whatever B is. Any condition works, not only equality.
    ///
    /// The inner is closed and reopened for each block, so its own counters
    /// add up over every scan (its tuples_out = |S| * number of blocks).
    class NestedLoopJoinOperator : public Operator
    {
    public:
        /// Binds `condition` against the two inputs now: unknown column ->
        /// Error(Name) / Error(Ambiguity), INT against VARCHAR -> Error(Type),
        /// the same qualified name on both sides -> Error(Schema). Output
        /// schema = left columns, then right columns (§M2.1). No page is read.
        NestedLoopJoinOperator(ExecContext& ctx, std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                               const Condition& condition, const FrameShare& share);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// Empties the block and fills it with the next outer tuples, until
        /// the next one does not fit or the outer ends. Returns false if the
        /// block stays empty (the outer is used up: the join is done).
        bool fillBlock();

        BoundCondition condition_;
        /// The outer block. Its pages are frames taken out of the pool.
        BoundedTupleBuffer block_;
        /// The outer tuple that did not fit in the last block; it starts the next one.
        Tuple carry_;
        bool hasCarry_ = false;
        /// The outer returned END (and was closed).
        bool outerDone_ = false;

        // Where next() stopped inside the pair loops, so the next call goes
        // on from there:
        /// The inner is open for the current block.
        bool innerOpen_ = false;
        /// The current inner tuple, tested against every tuple of the block.
        Tuple inner_;
        /// The block tuple to test against inner_ next. Equal to the block's
        /// size when inner_ is done: then the next inner tuple is read.
        std::size_t blockIndex_ = 0;
    };
} // namespace db::exec
