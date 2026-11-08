#pragma once
/// @file
/// Grace hash join (SPEC §M2.3 hash_join, chunk 13).

#include <cstddef>
#include <memory>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"
#include "db/exec/tuple_buffer.h"
#include "db/storage/temp_file.h"
#include "join/join_keys.h"
#include "sort/run_file.h"

namespace db::exec
{
    /// §M2.3's algorithm, left = build side:
    ///
    ///   partition phase (open): P = share.ownFrames (B - 1 for two scans)
    ///       scan R, write each tuple to partition h1(key) mod P; then S
    ///   probe phase (next): for i in 0 .. P-1
    ///       build a hash table on R_i using h2; scan S_i, probe, emit matches
    ///
    /// Equal keys hash the same (same types, same bytes, chunk 09), so
    /// matching tuples always share a partition number.
    ///
    /// Overflow (chunk 13 Q1 (b), user's choice): the table gets the stream
    /// share minus 1 reader page. Whether R_i fits is known before building,
    /// from its page count, so the table never grows past its frames. If R_i
    /// is bigger, that one pair is joined by block nested loop instead:
    /// blocks of (stream share - 2) pages of R_i, S_i read again for each
    /// block, every pair compared. Cost of that pair: R_i + ceil(R_i /
    /// block) * S_i reads. Rehashing would not help the skew of §M2.6
    /// measurement 4: every copy of one key lands in one partition.
    ///
    /// Cost without overflow: 3(M + N) = partitioning reads M + N (charged
    /// to the children) and writes M + N, probing reads M + N (this node).
    /// Comparisons: one per key comparison with a table entry of the probing
    /// tuple's bucket (the hash itself is not a comparison), or one per pair
    /// in a block nested loop.
    class HashJoinOperator : public Operator
    {
    public:
        /// Checks the condition now (equiJoinKeyColumns: equalities between
        /// left and right columns only, same types). Output schema = left
        /// columns, then right columns. No page is read.
        HashJoinOperator(ExecContext& ctx, std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                         const Condition& condition, const FrameShare& share);

        /// {"partitions": P, "left_pages": [...], "right_pages": [...],
        ///  "overflow_partitions": [...], "overflow_strategy":
        ///  "block_nested_loop"} of the last run (§M2.6 measurement 4).
        PlanJson details() const override;

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// Writes every tuple of `input` to P partitions by h1 of its key.
        std::vector<storage::TempFile> partition(Operator& input, const std::vector<std::size_t>& keyColumns);
        /// Gets partition pair part_ ready: a hash table on R_i, or (overflow)
        /// the first block of R_i; then a reader on S_i.
        void startPartition();
        /// Overflow only: the next block of R_i. False when R_i is used up.
        bool fillBlock();
        /// Deletes partition pair part_ and moves to the next one.
        void finishPartition();

        JoinKeys keys_;
        FrameShare share_;
        /// The partitions (declared before the readers on purpose: a reader
        /// unpins its page before its run is deleted).
        std::vector<storage::TempFile> leftParts_;
        std::vector<storage::TempFile> rightParts_;
        /// For details(): pages per partition and the overflowed partitions.
        std::vector<std::size_t> leftPages_;
        std::vector<std::size_t> rightPages_;
        std::vector<std::size_t> overflowed_;

        /// The partition pair being joined, and whether it is started.
        std::size_t part_ = 0;
        bool partStarted_ = false;
        /// True when R_i did not fit: block nested loop on this pair.
        bool blockMode_ = false;
        /// The R_i tuples in memory: the hash table's entries, or the block.
        std::unique_ptr<BoundedTupleBuffer> entries_;
        /// Hash table only: bucket k lists the entries whose h2 mod the
        /// bucket count is k. Numbers, not tuples (chunk 09 Q2 (a)).
        std::vector<std::vector<std::size_t>> buckets_;
        /// Block mode: the reader on R_i (kept between blocks), the R_i tuple
        /// that did not fit in the last block, and whether R_i is used up.
        std::unique_ptr<RunReader> buildReader_;
        Tuple carry_;
        bool hasCarry_ = false;
        bool buildDone_ = false;
        /// The reader on S_i, its current tuple, and the next candidate
        /// entry to compare it with (a position in its bucket, or in the block).
        std::unique_ptr<RunReader> probeReader_;
        Tuple probe_;
        bool hasProbe_ = false;
        std::size_t candidate_ = 0;
    };
} // namespace db::exec
