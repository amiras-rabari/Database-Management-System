#pragma once

// distinct in hash mode (SPEC §M2.3: "partitions to disk when the table of
// seen tuples exceeds its frames, then deduplicates partition by
// partition"). Private to exec; plans reach it through buildExecutablePlan.

#include <cstddef>
#include <cstdint>
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
#include "sort/run_file.h"

namespace db::exec
{
    /// Returns each different input tuple once. Schema = the child's.
    ///
    /// open(): every input tuple goes into a table of seen tuples
    /// (ownFrames pages; the child streams beside it).
    /// - If the input ends and the table also fits in the stream frames,
    ///   nothing is written: next() returns the table's entries in the order
    ///   they first appeared, with no I/O.
    /// - Otherwise (the table filled up): the table is written to one spill
    ///   run, the rest of the input goes into P = ownFrames partitions by h1,
    ///   the child is closed, and the spill run is sent into the
    ///   partitions too. Equal tuples always share a partition, so removing
    ///   duplicates inside each partition removes them all.
    ///
    /// next(), one partition at a time: a fresh table of streamFrames - 1
    /// pages (+1 frame to read the partition). If the partition has more
    /// different tuples than that, it is read again into an external sort
    /// and adjacent equals are skipped (chunk 09 Q1 (a)). Each partition is
    /// deleted once done.
    ///
    /// Extra cost on overflow: the spill run, written and read once
    /// (Q3 (a)); a sorted partition is read twice.
    class HashDistinctOperator : public Operator
    {
    public:
        /// No checks needed (every column is a key). No page is read.
        HashDistinctOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const FrameShare& share);

        /// How many partitions needed the sort fallback, over every open so
        /// far (for tests; the report reads it through details() later).
        std::size_t sortedPartitions() const { return sortedPartitions_; }

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// Writes the table's entries to a new run and empties the table.
        storage::TempFile spillTable();
        /// Gets partitions_[nextPartition_] ready to be returned: in a table
        /// if it fits, else in a sorter. Returns false when none is left.
        bool startNextPartition();

        FrameShare share_;
        std::size_t sortedPartitions_ = 0;

        /// The partitions still to do (empty when everything stayed in
        /// memory), and the next one to start.
        std::vector<storage::TempFile> partitions_;
        std::size_t nextPartition_ = 0;

        /// What next() returns from now: the table's entries (from
        /// position `nextEntry_`), or the sorter's output with adjacent
        /// equals skipped.
        std::optional<TupleHashTable> table_;
        std::size_t nextEntry_ = 0;
        std::optional<ExternalSorter> sorter_;
        Tuple last_;
        bool hasLast_ = false;
    };
} // namespace db::exec
