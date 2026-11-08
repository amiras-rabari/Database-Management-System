#include "join/hash_join.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "db/storage/storage_engine.h"
#include "join/hash_partitioner.h"
#include "join/hashing.h"

namespace db::exec
{
    namespace
    {
        // Buckets per page of table entries (the same as chunk 09's table).
        constexpr std::size_t kBucketsPerPage = 512;
    } // namespace

    HashJoinOperator::HashJoinOperator(ExecContext& ctx, std::unique_ptr<Operator> left,
                                       std::unique_ptr<Operator> right, const Condition& condition,
                                       const FrameShare& share)
        : Operator(ctx, twoChildren(std::move(left), std::move(right))),
          keys_(equiJoinKeyColumns(condition, child(0).schema(), child(1).schema(), "hash_join")), share_(share)
    {
        setSchema(concatSchemas(child(0).schema(), child(1).schema()));
    }

    std::vector<storage::TempFile> HashJoinOperator::partition(Operator& input,
                                                               const std::vector<std::size_t>& keyColumns)
    {
        // P writer pages beside the input's own frames (phase rule).
        HashPartitioner partitioner(ctx().engine(), input.schema(), keyColumns, share_.ownFrames, kSeedPartition);
        input.open();
        Tuple t;
        while (input.next(t))
            partitioner.add(t);
        input.close();
        return partitioner.finish();
    }

    void HashJoinOperator::doOpen()
    {
        // Partition phase: the left input, then the right one, each with the
        // same h1 and P. Both are always partitioned, so the cost stays 3(M + N).
        leftParts_ = partition(child(0), keys_.leftColumns);
        rightParts_ = partition(child(1), keys_.rightColumns);
        leftPages_.clear();
        rightPages_.clear();
        overflowed_.clear();
        for (std::size_t i = 0; i < leftParts_.size(); ++i)
        {
            leftPages_.push_back(leftParts_[i].numPages());
            rightPages_.push_back(rightParts_[i].numPages());
        }
        part_ = 0;
        partStarted_ = false;
    }

    void HashJoinOperator::startPartition()
    {
        const storage::TempFile& leftPart = leftParts_[part_];
        const std::size_t tablePages = share_.streamFrames - 1; // 1 frame for the S_i reader
        blockMode_ = leftPart.numPages() > tablePages;
        if (!blockMode_)
        {
            // Build: every R_i tuple into the table. It fits: the table counts
            // pages the way run pages fill, and R_i has at most tablePages.
            entries_ = std::make_unique<BoundedTupleBuffer>(ctx().pool(), child(0).schema(), tablePages);
            buckets_.assign(tablePages * kBucketsPerPage, {});
            RunReader reader(ctx().engine(), leftPart, child(0).schema());
            Tuple t;
            while (reader.next(t))
            {
                if (!entries_->tryAdd(t))
                    throw std::logic_error("hash_join: a partition that fits by its page count overflowed its table");
                const std::size_t bucket = hashKey(t, keys_.leftColumns, kSeedTable) % buckets_.size();
                buckets_[bucket].push_back(entries_->size() - 1);
            }
        }
        else
        {
            // Overflow: block nested loop on this pair. Block + R_i reader +
            // S_i reader = the stream share.
            overflowed_.push_back(part_);
            entries_ = std::make_unique<BoundedTupleBuffer>(ctx().pool(), child(0).schema(),
                                                            share_.streamFrames - 2);
            buildReader_ = std::make_unique<RunReader>(ctx().engine(), leftPart, child(0).schema());
            hasCarry_ = false;
            buildDone_ = false;
            fillBlock();
        }
        probeReader_ = std::make_unique<RunReader>(ctx().engine(), rightParts_[part_], child(1).schema());
        hasProbe_ = false;
        partStarted_ = true;
    }

    bool HashJoinOperator::fillBlock()
    {
        entries_->clear();
        if (hasCarry_)
        {
            entries_->tryAdd(carry_); // one tuple always fits in an empty block
            hasCarry_ = false;
        }
        Tuple t;
        while (!buildDone_)
        {
            if (!buildReader_->next(t))
            {
                buildDone_ = true;
                buildReader_.reset();
                break;
            }
            if (!entries_->tryAdd(t))
            {
                carry_ = std::move(t);
                hasCarry_ = true;
                break;
            }
        }
        return entries_->size() > 0;
    }

    void HashJoinOperator::finishPartition()
    {
        // Readers first: they may hold a page of the partitions.
        probeReader_.reset();
        buildReader_.reset();
        entries_.reset();
        buckets_.clear();
        leftParts_[part_].drop();
        rightParts_[part_].drop();
        partStarted_ = false;
        ++part_;
    }

    bool HashJoinOperator::doNext(Tuple& out)
    {
        std::uint64_t& comparisons = mutableCounters().comparisons;
        while (true)
        {
            if (!partStarted_)
            {
                if (part_ == leftParts_.size())
                    return false; // every partition pair is done
                startPartition();
            }
            if (!hasProbe_)
            {
                hasProbe_ = probeReader_->next(probe_);
                candidate_ = 0;
                if (!hasProbe_)
                {
                    // S_i is used up. In block mode, read it again for the
                    // next block of R_i; otherwise this pair is done.
                    if (blockMode_ && fillBlock())
                        probeReader_ = std::make_unique<RunReader>(ctx().engine(), rightParts_[part_],
                                                                   child(1).schema());
                    else
                        finishPartition();
                    continue;
                }
            }

            // Compare probe_ with its candidates: the entries of its bucket,
            // or every tuple of the block. Each comparison counts 1.
            const std::vector<Tuple>& entries = entries_->tuples();
            const std::vector<std::size_t>* bucket = nullptr;
            if (!blockMode_)
                bucket = &buckets_[hashKey(probe_, keys_.rightColumns, kSeedTable) % buckets_.size()];
            const std::size_t count = blockMode_ ? entries.size() : bucket->size();
            while (candidate_ < count)
            {
                const Tuple& entry = entries[blockMode_ ? candidate_ : (*bucket)[candidate_]];
                ++candidate_;
                if (compareKeys(entry, keys_.leftColumns, probe_, keys_.rightColumns, comparisons) == 0)
                {
                    // Output = the left (build) tuple, then the right one (§M2.1).
                    out = entry;
                    out.insert(out.end(), probe_.begin(), probe_.end());
                    return true;
                }
            }
            hasProbe_ = false;
        }
    }

    void HashJoinOperator::doClose()
    {
        probeReader_.reset();
        buildReader_.reset();
        entries_.reset();
        buckets_.clear();
        leftParts_.clear();
        rightParts_.clear();
        partStarted_ = false;
    }

    PlanJson HashJoinOperator::details() const
    {
        PlanJson details = PlanJson::object();
        details["partitions"] = leftPages_.size();
        details["left_pages"] = leftPages_;
        details["right_pages"] = rightPages_;
        details["overflow_partitions"] = overflowed_;
        details["overflow_strategy"] = "block_nested_loop";
        return details;
    }
} // namespace db::exec
