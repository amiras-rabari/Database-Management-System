#pragma once

// distinct and group_by in hash mode (SPEC §M2.3: distinct "partitions to
// disk when the table of seen tuples exceeds its frames, then deduplicates
// partition by partition"; group_by works the same way with groups).
// Private to exec; plans reach it through buildExecutablePlan.

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/external_sort.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"
#include "db/storage/temp_file.h"
#include "join/tuple_hash_table.h"
#include "operators/grouping.h"

namespace db::exec
{
    /// One output row per group. distinct is the grouping where every
    /// column is a key and there are no aggregates (distinctGrouping), so
    /// it returns each different input tuple once.
    ///
    /// The table holds one state row per group (operators/grouping.h), in
    /// the order the groups first appeared. A row whose key is already in
    /// the table is merged into that group's state row.
    ///
    /// open(): every input row goes into the table (ownFrames pages; the
    /// child streams beside it).
    /// - If the input ends and the table also fits in the stream frames,
    ///   nothing is written: next() returns the groups in the order they
    ///   first appeared, with no I/O.
    /// - Otherwise (the table filled up): the table is written to one spill
    ///   run, the rest of the input goes into P = ownFrames partitions by h1
    ///   of the key, the child is closed, and the spill run is sent into the
    ///   partitions too. Rows of one group always share a partition, so
    ///   each partition can be grouped alone.
    ///
    /// next(), one partition at a time: a fresh table of streamFrames - 1
    /// pages (+1 frame to read the partition). If the partition has more
    /// groups than that, it is read again into an external sort on the key
    /// and neighbours with the same key are merged (chunk 09 Q1 (a)). Each
    /// partition is deleted once done.
    ///
    /// Extra cost on overflow: the spill run, written and read once
    /// (chunk 09 Q3 (a)); a sorted partition is read twice.
    class HashGroupOperator : public Operator
    {
    public:
        /// `grouping` must come from makeGrouping or distinctGrouping on the
        /// child's schema (they did every check). No page is read.
        HashGroupOperator(ExecContext& ctx, std::unique_ptr<Operator> child, Grouping grouping,
                          const FrameShare& share);

        /// How many partitions needed the sort fallback, over every open so
        /// far (for tests; the report reads it through details() later).
        std::size_t sortedPartitions() const { return sortedPartitions_; }

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// Puts one state row into the table: a new group, or merged into
        /// its group. Returns false (nothing changed) if the table is full.
        bool addToTable(const Tuple& state);
        /// Writes the table's entries to a new run and empties the table.
        storage::TempFile spillTable();
        /// Gets partitions_[nextPartition_] ready to be returned: in a table
        /// if it fits, else in a sorter. Returns false when none is left.
        bool startNextPartition();

        Grouping grouping_;
        FrameShare share_;
        std::size_t sortedPartitions_ = 0;

        /// The partitions still to do (empty when everything stayed in
        /// memory), and the next one to start.
        std::vector<storage::TempFile> partitions_;
        std::size_t nextPartition_ = 0;

        /// What next() returns from now: the table's groups (from position
        /// `nextEntry_`), or the sorter's output with neighbours of the same
        /// key merged (`current_` is the group being gathered).
        std::optional<TupleHashTable> table_;
        std::size_t nextEntry_ = 0;
        std::optional<ExternalSorter> sorter_;
        Tuple current_;
        bool hasCurrent_ = false;

        /// No keys and an empty input: next() returns emptyInputRow once.
        bool returnEmptyRow_ = false;
    };
} // namespace db::exec
