#pragma once

// Sort-based duplicate removal (SPEC §M2.3, chunk 08): distinct in sort
// mode, union, intersect and except. All have SET semantics: every output
// tuple appears once (C20; the module doc wins over the handout's "bag and
// set versions", so there is no bag intersect/except). Private to exec;
// plans reach them through buildExecutablePlan.
//
// They work on inputs much larger than B: the only memory they use is the
// sorter's frames, plus one remembered tuple or two run readers.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/external_sort.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"
#include "db/storage/storage_engine.h"

namespace db::exec
{
    /// Sort keys for every column of `schema`, in order, ascending: after a
    /// sort on them, equal tuples are next to each other.
    std::vector<SortKey> allColumnsAscending(const Schema& schema);

    /// The same for the first `count` columns only (a group's key comes
    /// first in group_by's rows, chunk 10).
    std::vector<SortKey> firstColumnsAscending(const Schema& schema, std::size_t count);

    /// Compares two tuples column by column, by position: negative if `a`
    /// comes first, 0 if equal, positive if `b` comes first (the order of
    /// allColumnsAscending). Adds 1 to `comparisons`. Both tuples must have
    /// the same types position by position (checked when the plan is built).
    int compareRows(const Tuple& a, const Tuple& b, std::uint64_t& comparisons);

    /// Like compareRows, but only on the first `count` columns: a group's
    /// key comes first in group_by's rows (chunk 10). Counts 1 comparison.
    int compareFirstColumns(const Tuple& a, const Tuple& b, std::size_t count, std::uint64_t& comparisons);

    /// distinct (sort mode, one child) and union (two children). Every input
    /// tuple goes into ONE sorter, sorted on all columns; equal tuples then
    /// come out next to each other, so next() only has to skip a tuple equal
    /// to the one it returned last. For union both inputs go into the same
    /// sorter (Q1 (a)), so a tuple on both sides is removed too.
    ///
    /// Cost: what the sort costs, plus one comparison per output check.
    /// Frames (FrameShare, phase rule): pass 0 ownFrames, merges openFrames,
    /// final merge streamFrames, as in SortOperator.
    class DedupSortOperator : public Operator
    {
    public:
        /// With two children (union): checks they are union-compatible now,
        /// while the plan is built: same column count and the same type at
        /// each position, else db::Error(Schema). Output schema = the first
        /// child's (union takes the left input's names). No page is read.
        DedupSortOperator(ExecContext& ctx, Children children, const FrameShare& share);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// The schema the sorter stores tuples with. For union, the right
        /// input may have a longer VARCHAR than the left (allowed), so each
        /// VARCHAR length is the larger of the two; else run pages would
        /// refuse a long right value.
        Schema sortSchema_;
        FrameShare share_;
        /// Exists only between open and close; each open makes a new one.
        std::optional<ExternalSorter> sorter_;
        /// The tuple returned last, to skip its duplicates. One tuple, not a
        /// container.
        Tuple last_;
        bool hasLast_ = false;
    };

    /// For an operator that sorts two inputs and then merges them side by
    /// side (intersect, except, sort-merge join): opens `input`, adds every
    /// tuple to a new sorter on `keys`, closes `input`, then finishToRuns.
    /// When it returns the sorter holds no frame (its runs are on disk), so
    /// the other input can be sorted with all the frames; call
    /// startFinalMerge() on both sorters afterwards. Frames: pass 0 runs of
    /// share.ownFrames pages, merges of share.openFrames, a final merge of at
    /// most finalFrames - 1 runs. Every comparison adds 1 to `comparisons`.
    std::unique_ptr<ExternalSorter> sortToRuns(storage::StorageEngine& engine, Operator& input,
                                               const std::vector<SortKey>& keys, const FrameShare& share,
                                               std::size_t finalFrames, std::uint64_t& comparisons);

    /// intersect and except. Each input is sorted on all columns
    /// (sortToRuns): first the left input (then it is closed), then the
    /// right one. Both sorts then stream their final merge side by side
    /// into this operator's merge, so nothing is written beyond the sorts'
    /// own runs (§M2.1):
    /// - intersect: a value on both sides is returned once;
    /// - except: a left value that is not on the right is returned once.
    /// Cost: sort(M) + sort(N). Every head comparison counts 1, on top of
    /// the sorts'. Frames while streaming: the stream share, split between
    /// the two final merges (at least 1 run each).
    class SetMergeOperator : public Operator
    {
    public:
        enum class Kind
        {
            Intersect,
            Except
        };

        /// Checks the inputs are union-compatible now (db::Error(Schema)
        /// otherwise). Output schema = the left input's. No page is read.
        SetMergeOperator(ExecContext& ctx, std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                         Kind kind, const FrameShare& share);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// Moves the left (or right) sorter past every tuple equal to `value`.
        void skipLeft(const Tuple& value);
        void skipRight(const Tuple& value);

        Kind kind_;
        FrameShare share_;
        /// The two sorts; they exist only between open and close.
        std::unique_ptr<ExternalSorter> leftSorter_;
        std::unique_ptr<ExternalSorter> rightSorter_;
        /// Each sort's current (head) tuple, and whether it still has one.
        Tuple leftHead_;
        Tuple rightHead_;
        bool hasLeft_ = false;
        bool hasRight_ = false;
    };
} // namespace db::exec
