#pragma once
/// @file
/// Sort-merge join (SPEC §M2.3 sort_merge_join, chunk 12).

#include <cstddef>
#include <memory>
#include <optional>

#include "db/common/condition.h"
#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/external_sort.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"
#include "db/exec/tuple_buffer.h"
#include "db/storage/temp_file.h"
#include "join/join_keys.h"
#include "sort/run_file.h"

namespace db::exec
{
    /// Sorts both inputs on the join key with the external sort, then walks
    /// them side by side, exactly as §M2.3's pseudocode:
    ///
    ///   r = first of R, s = first of S
    ///   while r and s:
    ///       if r.key < s.key: advance r;  elif r.key > s.key: advance s
    ///       else: for each r' with this key: rewind s to the first s of
    ///             the run, emit (r', s') for each s' of the run
    ///
    /// Both sorts stream their final merge into this merge (they are "your
    /// sort operator", §M2.3), so the right input cannot be read twice by
    /// itself. The "rewind" is over the current run of equal right keys,
    /// which is kept in 1 page; a longer run is spilled to a temporary run
    /// and re-read from there for each r' (§M2.3: "spilling the inner run to
    /// a temp file is acceptable; assuming it fits is not").
    ///
    /// Example (the spec's): R.b = [1, 2, 2, 3], S.b = [2, 2, 4]. 1 < 2:
    /// advance r. 2 = 2: the run of S is [2, 2]; r = 2 gives 2 rows; the
    /// next r is 2 again, so rewind and give 2 more; r = 3 ends the run.
    /// 3 < 4, then R ends. 4 rows.
    ///
    /// Cost: sort(M) + sort(N), as M4's sort formula counts them (their final
    /// pass is the merge's only read of each input), plus re-reads of a
    /// spilled run. Comparisons: the sorts' plus one per key comparison in
    /// the merge, all on this node (§M2.3). Frames while streaming: both
    /// final merges + 1 page for the run of equal keys = 3 at least.
    class SortMergeJoinOperator : public Operator
    {
    public:
        /// Checks the condition now (equiJoinKeyColumns: equalities between
        /// left and right columns only, same types). Output schema = left
        /// columns, then right columns. No page is read.
        SortMergeJoinOperator(ExecContext& ctx, std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                              const Condition& condition, const FrameShare& share);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// Reads the whole run of right tuples equal to rightHead_ into the
        /// group (spilling it if needed); rightHead_ ends on the first
        /// tuple after the run.
        void collectGroup();
        /// Adds one right tuple to the group, spilling the group to a
        /// temporary run when it outgrows its page.
        void addToGroup(const Tuple& t);
        /// Starts the group again from its first tuple (the "rewind").
        void rewindGroup();
        /// The next tuple of the group, or false at its end.
        bool nextInGroup(Tuple& out);
        /// Forgets the group (and deletes its spill run).
        void endGroup();

        JoinKeys keys_;
        FrameShare share_;
        std::unique_ptr<ExternalSorter> leftSorter_;
        std::unique_ptr<ExternalSorter> rightSorter_;
        /// The current tuple of each sorted input, and whether there is one.
        Tuple leftHead_;
        Tuple rightHead_;
        bool hasLeft_ = false;
        bool hasRight_ = false;

        /// The current run of equal right keys: in group_ (1 page) or, once
        /// it outgrew that page, in groupRun_. The writer exists only while a
        /// spilled run is being collected. The run is declared before its
        /// reader, so the reader unpins its page before the run is deleted.
        BoundedTupleBuffer group_;
        std::unique_ptr<RunWriter> groupWriter_;
        std::optional<storage::TempFile> groupRun_;
        std::unique_ptr<RunReader> groupReader_;
        /// The group's key (its first tuple), and the next group tuple to
        /// pair with leftHead_ when the group is in group_.
        Tuple groupKey_;
        std::size_t groupIndex_ = 0;
        /// leftHead_ is being paired with the group.
        bool emitting_ = false;
    };
} // namespace db::exec
