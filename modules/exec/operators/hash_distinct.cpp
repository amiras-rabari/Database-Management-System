#include "operators/hash_distinct.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "db/exec/tuple_buffer.h"
#include "join/hash_partitioner.h"
#include "join/hashing.h"
#include "sort/set_ops.h"

namespace db::exec
{
    namespace
    {
        // Positions 0 .. n-1: distinct's key is the whole tuple.
        std::vector<std::size_t> allPositions(const Schema& schema)
        {
            std::vector<std::size_t> positions;
            for (std::size_t i = 0; i < schema.size(); ++i)
                positions.push_back(i);
            return positions;
        }
    } // namespace

    HashDistinctOperator::HashDistinctOperator(ExecContext& ctx, std::unique_ptr<Operator> child,
                                               const FrameShare& share)
        : Operator(ctx, oneChild(std::move(child))), share_(share)
    {
        setSchema(this->child(0).schema());
    }

    storage::TempFile HashDistinctOperator::spillTable()
    {
        // Same trick as a sort run (chunk 07 Q3 (a)): the page being written
        // is one of the table's frames, and each tuple leaves the table as it
        // goes onto the page, so memory never grows. The table is not empty:
        // it filled up or uses more pages than the stream frames.
        BoundedTupleBuffer& entries = table_->entries();
        entries.giveFrameToOutput();
        RunWriter writer(ctx().engine(), schema());
        while (entries.size() > 0)
        {
            writer.append(entries.tuples().back());
            entries.popBack();
        }
        table_->clearBuckets();
        return writer.finish();
    }

    void HashDistinctOperator::doOpen()
    {
        partitions_.clear();
        nextPartition_ = 0;
        sorter_.reset();
        nextEntry_ = 0;
        table_.emplace(ctx().pool(), schema(), share_.ownFrames, mutableCounters().comparisons);

        // Made only when the table fills up. P = ownFrames: while the child
        // still streams, it uses the rest of the open frames (phase rule).
        std::optional<HashPartitioner> partitioner;
        std::optional<storage::TempFile> spill;

        child(0).open();
        Tuple t;
        while (child(0).next(t))
        {
            if (!partitioner)
            {
                if (table_->insertIfNew(t) != TupleHashTable::InsertResult::Full)
                    continue; // new and kept, or seen before
                // The table is full: switch to partitioning. The table goes
                // to disk first, so its frames are free for the P writers.
                spill = spillTable();
                partitioner.emplace(ctx().engine(), schema(), allPositions(schema()), share_.ownFrames,
                                    kSeedPartition);
            }
            partitioner->add(t);
        }
        child(0).close();

        if (!partitioner)
        {
            // Everything fit. Keep it in memory only if next() may hold that
            // many frames too (a parent may give this node fewer frames to
            // stream with than to open with); else partition it after all.
            if (table_->entries().pagesUsed() <= share_.streamFrames)
                return;
            spill = spillTable();
            partitioner.emplace(ctx().engine(), schema(), allPositions(schema()), share_.ownFrames,
                                kSeedPartition);
        }

        // The spilled tuples go into the partitions too. Frames: P writers
        // + 1 reader, the child is closed by now.
        {
            RunReader reader(ctx().engine(), *spill, schema());
            while (reader.next(t))
                partitioner->add(t);
        }
        spill.reset(); // deletes the spill run
        partitions_ = partitioner->finish();
        table_.reset(); // next() starts on partition 0
    }

    bool HashDistinctOperator::startNextPartition()
    {
        if (nextPartition_ == partitions_.size())
            return false;
        storage::TempFile& partition = partitions_[nextPartition_];
        ++nextPartition_;

        // A fresh table on this partition: the stream frames minus one for
        // the reader. h2 (the table's seed) differs from h1, so the tuples
        // of one partition still spread over all buckets.
        const std::size_t tableFrames = share_.streamFrames - 1;
        table_.emplace(ctx().pool(), schema(), tableFrames, mutableCounters().comparisons);
        nextEntry_ = 0;
        bool fits = true;
        {
            RunReader reader(ctx().engine(), partition, schema());
            Tuple t;
            while (fits && reader.next(t))
                fits = table_->insertIfNew(t) != TupleHashTable::InsertResult::Full;
        }
        if (fits)
        {
            partition.drop(); // its tuples are all in the table now
            return true;
        }

        // Too many different tuples for the table (Q1 (a)): forget the table
        // and read the partition again into an external sort, which always
        // fits. Reader 1 + pass 0's run tableFrames; merges use every
        // stream frame once the reader is gone.
        table_.reset();
        ++sortedPartitions_;
        sorter_.emplace(ctx().engine(), schema(), allColumnsAscending(schema()), tableFrames, share_.streamFrames,
                        share_.streamFrames, mutableCounters().comparisons);
        {
            RunReader reader(ctx().engine(), partition, schema());
            Tuple t;
            while (reader.next(t))
                sorter_->add(t);
        }
        partition.drop();
        sorter_->finishInput();
        hasLast_ = false;
        return true;
    }

    bool HashDistinctOperator::doNext(Tuple& out)
    {
        while (true)
        {
            if (sorter_)
            {
                // A sorted partition: equal tuples are next to each other.
                Tuple t;
                while (sorter_->next(t))
                {
                    if (hasLast_ && compareRows(t, last_, mutableCounters().comparisons) == 0)
                        continue;
                    last_ = t;
                    hasLast_ = true;
                    out = std::move(t);
                    return true;
                }
                sorter_.reset();
            }
            else if (table_)
            {
                // A table: every entry is different; return them in the
                // order they were inserted.
                const std::vector<Tuple>& entries = table_->entries().tuples();
                if (nextEntry_ < entries.size())
                {
                    out = entries[nextEntry_];
                    ++nextEntry_;
                    return true;
                }
                table_.reset();
            }
            if (!startNextPartition())
                return false;
        }
    }

    void HashDistinctOperator::doClose()
    {
        // Gives back every frame and deletes every partition left (if the
        // parent stopped early).
        sorter_.reset();
        table_.reset();
        partitions_.clear();
    }
} // namespace db::exec
