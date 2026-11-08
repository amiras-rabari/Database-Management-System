#include "join/tuple_hash_table.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "join/hashing.h"
#include "sort/set_ops.h"

namespace db::exec
{
    TupleHashTable::TupleHashTable(storage::BufferPool& pool, const Schema& schema, std::size_t keyCount,
                                   std::size_t capacityPages, bool chargeLargestSize, std::uint64_t& comparisons)
        : entries_(pool, schema, capacityPages, chargeLargestSize), comparisons_(comparisons)
    {
        // At most 511 entries fit on a page (one INT: 4 + 4-byte slot = 8
        // bytes, 4088 / 8), so 512 buckets per page give about one entry per
        // bucket at most. Fixed up front: the table never has to rehash.
        buckets_.resize(capacityPages * 512);
        for (std::size_t i = 0; i < keyCount; ++i)
            keyColumns_.push_back(i);
    }

    TupleHashTable::InsertResult TupleHashTable::insertIfNew(const Tuple& tuple)
    {
        std::vector<std::size_t>& bucket = buckets_[hashKey(tuple, keyColumns_, kSeedTable) % buckets_.size()];
        // Only the entries in this bucket can have an equal key.
        for (std::size_t entry : bucket)
        {
            if (compareFirstColumns(entries_.tuples()[entry], tuple, keyColumns_.size(), comparisons_) == 0)
            {
                foundEntry_ = entry;
                return InsertResult::Duplicate;
            }
        }
        const std::size_t position = entries_.size();
        if (!entries_.tryAdd(tuple))
            return InsertResult::Full;
        bucket.push_back(position);
        return InsertResult::Added;
    }

    void TupleHashTable::clearBuckets()
    {
        for (std::vector<std::size_t>& bucket : buckets_)
            bucket.clear();
    }
} // namespace db::exec
