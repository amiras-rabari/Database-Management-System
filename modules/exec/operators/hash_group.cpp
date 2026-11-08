#include "operators/hash_group.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "db/exec/tuple_buffer.h"
#include "join/hash_partitioner.h"
#include "join/hashing.h"
#include "sort/run_file.h"
#include "sort/set_ops.h"

namespace db::exec
{
    namespace
    {
        // Positions 0 .. count-1: a state row starts with its key columns.
        std::vector<std::size_t> firstPositions(std::size_t count)
        {
            std::vector<std::size_t> positions;
            for (std::size_t i = 0; i < count; ++i)
                positions.push_back(i);
            return positions;
        }
    } // namespace

    HashGroupOperator::HashGroupOperator(ExecContext& ctx, std::unique_ptr<Operator> child, Grouping grouping,
                                         const FrameShare& share)
        : Operator(ctx, oneChild(std::move(child))), grouping_(std::move(grouping)), share_(share)
    {
        setSchema(grouping_.outputSchema);
    }

    bool HashGroupOperator::addToTable(const Tuple& state)
    {
        const TupleHashTable::InsertResult result = table_->insertIfNew(state);
        if (result == TupleHashTable::InsertResult::Duplicate)
        {
            // Changed in place: the table counts every entry at its largest
            // size when a state row can grow (Grouping::stateCanGrow).
            Tuple& group = table_->entries().tuples()[table_->foundEntry()];
            mergeStateRow(grouping_, group, state, mutableCounters().comparisons);
        }
        return result != TupleHashTable::InsertResult::Full;
    }

    storage::TempFile HashGroupOperator::spillTable()
    {
        // Same trick as a sort run (chunk 07 Q3 (a)): the page being written
        // is one of the table's frames, and each row leaves the table as it
        // goes onto the page, so memory never grows. The table is not empty:
        // it filled up or uses more pages than the stream frames.
        BoundedTupleBuffer& entries = table_->entries();
        entries.giveFrameToOutput();
        RunWriter writer(ctx().engine(), grouping_.stateSchema);
        while (entries.size() > 0)
        {
            writer.append(entries.tuples().back());
            entries.popBack();
        }
        table_->clearBuckets();
        return writer.finish();
    }

    void HashGroupOperator::doOpen()
    {
        partitions_.clear();
        nextPartition_ = 0;
        sorter_.reset();
        hasCurrent_ = false;
        nextEntry_ = 0;
        returnEmptyRow_ = false;
        const std::size_t keyCount = grouping_.keyColumns.size();
        table_.emplace(ctx().pool(), grouping_.stateSchema, keyCount, share_.ownFrames, grouping_.stateCanGrow,
                       mutableCounters().comparisons);

        // Made only when the table fills up. P = ownFrames: while the child
        // still streams, it uses the rest of the open frames (phase rule).
        std::optional<HashPartitioner> partitioner;
        std::optional<storage::TempFile> spill;

        child(0).open();
        Tuple t;
        while (child(0).next(t))
        {
            const Tuple state = toStateRow(grouping_, t);
            if (!partitioner)
            {
                if (addToTable(state))
                    continue; // a new group, or added to its group
                // The table is full: switch to partitioning. The table goes
                // to disk first, so its frames are free for the P writers.
                spill = spillTable();
                partitioner.emplace(ctx().engine(), grouping_.stateSchema, firstPositions(keyCount),
                                    share_.ownFrames, kSeedPartition);
            }
            partitioner->add(state);
        }
        child(0).close();

        if (!partitioner)
        {
            // §M2.3: no keys and no input rows still gives one row.
            returnEmptyRow_ = keyCount == 0 && table_->entries().size() == 0;
            // Everything fit. Keep it in memory only if next() may hold that
            // many frames too (a parent may give this node fewer frames to
            // stream with than to open with); else partition it after all.
            if (table_->entries().pagesUsed() <= share_.streamFrames)
                return;
            spill = spillTable();
            partitioner.emplace(ctx().engine(), grouping_.stateSchema, firstPositions(keyCount), share_.ownFrames,
                                kSeedPartition);
        }

        // The spilled rows go into the partitions too. Frames: P writers
        // + 1 reader, the child is closed by now.
        {
            RunReader reader(ctx().engine(), *spill, grouping_.stateSchema);
            while (reader.next(t))
                partitioner->add(t);
        }
        spill.reset(); // deletes the spill run
        partitions_ = partitioner->finish();
        table_.reset(); // next() starts on partition 0
    }

    bool HashGroupOperator::startNextPartition()
    {
        if (nextPartition_ == partitions_.size())
            return false;
        storage::TempFile& partition = partitions_[nextPartition_];
        ++nextPartition_;
        const Schema& schema = grouping_.stateSchema;
        const std::size_t keyCount = grouping_.keyColumns.size();

        // A fresh table on this partition: the stream frames minus one for
        // the reader. h2 (the table's seed) differs from h1, so the rows of
        // one partition still spread over all buckets.
        const std::size_t tableFrames = share_.streamFrames - 1;
        table_.emplace(ctx().pool(), schema, keyCount, tableFrames, grouping_.stateCanGrow,
                       mutableCounters().comparisons);
        nextEntry_ = 0;
        bool fits = true;
        {
            RunReader reader(ctx().engine(), partition, schema);
            Tuple t;
            while (fits && reader.next(t))
                fits = addToTable(t);
        }
        if (fits)
        {
            partition.drop(); // its rows are all in the table now
            return true;
        }

        // Too many groups for the table (chunk 09 Q1 (a)): forget the table
        // and read the partition again into an external sort on the key,
        // which always fits. Reader 1 + pass 0's run tableFrames; merges use
        // every stream frame once the reader is gone.
        table_.reset();
        ++sortedPartitions_;
        sorter_.emplace(ctx().engine(), schema, firstColumnsAscending(schema, keyCount), tableFrames,
                        share_.streamFrames, share_.streamFrames, mutableCounters().comparisons);
        {
            RunReader reader(ctx().engine(), partition, schema);
            Tuple t;
            while (reader.next(t))
                sorter_->add(t);
        }
        partition.drop();
        sorter_->finishInput();
        hasCurrent_ = false;
        return true;
    }

    bool HashGroupOperator::doNext(Tuple& out)
    {
        if (returnEmptyRow_)
        {
            returnEmptyRow_ = false;
            out = emptyInputRow(grouping_);
            return true;
        }
        std::uint64_t& comparisons = mutableCounters().comparisons;
        while (true)
        {
            if (sorter_)
            {
                // A sorted partition: rows of one group are next to each
                // other. Merge while the key stays the same.
                Tuple t;
                while (sorter_->next(t))
                {
                    if (!hasCurrent_)
                    {
                        current_ = std::move(t);
                        hasCurrent_ = true;
                        continue;
                    }
                    if (compareFirstColumns(t, current_, grouping_.keyColumns.size(), comparisons) == 0)
                    {
                        mergeStateRow(grouping_, current_, t, comparisons);
                        continue;
                    }
                    out = finishStateRow(grouping_, current_);
                    current_ = std::move(t);
                    return true;
                }
                sorter_.reset();
                if (hasCurrent_)
                {
                    // The partition's last group.
                    hasCurrent_ = false;
                    out = finishStateRow(grouping_, current_);
                    return true;
                }
            }
            else if (table_)
            {
                // A table: one entry per group; return them in the order
                // they first appeared.
                const std::vector<Tuple>& entries = table_->entries().tuples();
                if (nextEntry_ < entries.size())
                {
                    out = finishStateRow(grouping_, entries[nextEntry_]);
                    ++nextEntry_;
                    return true;
                }
                table_.reset();
            }
            if (!startNextPartition())
                return false;
        }
    }

    void HashGroupOperator::doClose()
    {
        // Gives back every frame and deletes every partition left (if the
        // parent stopped early).
        sorter_.reset();
        table_.reset();
        partitions_.clear();
    }
} // namespace db::exec
