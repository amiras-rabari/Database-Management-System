#pragma once

// A small in-memory hash table of tuples that stays inside its frames
// (chunk 09). Hash distinct uses it to remember the tuples it has seen;
// hash group_by (chunk 10) keeps one row per group in it. Private to exec.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "db/common/schema.h"
#include "db/exec/tuple_buffer.h"
#include "db/storage/buffer_pool.h"

namespace db::exec
{
    /// The entries live in a BoundedTupleBuffer, in the order they were
    /// inserted, so they are counted in pages against the budget (§M2.1: no
    /// uncounted tuples) and come out in the same order on every platform
    /// (G4: no unordered-container order).
    ///
    /// Two entries are "equal" when their first `keyCount` columns are
    /// equal: every column for distinct, the group key for group_by (its
    /// rows start with the key columns).
    ///
    /// Beside them, a bucket array: bucket k lists the positions of the
    /// entries whose h2 hash mod the bucket count is k. It holds numbers,
    /// not tuples, and is not counted (chunk 09 Q2 (a), user's choice).
    class TupleHashTable
    {
    public:
        /// An empty table of at most `capacityPages` pages. Every tuple
        /// comparison during a lookup adds 1 to `comparisons`.
        /// `chargeLargestSize` is passed to the buffer: set it when entries
        /// will be changed in place (see BoundedTupleBuffer). `pool`,
        /// `schema` and `comparisons` must outlive the table.
        TupleHashTable(storage::BufferPool& pool, const Schema& schema, std::size_t keyCount,
                       std::size_t capacityPages, bool chargeLargestSize, std::uint64_t& comparisons);

        enum class InsertResult
        {
            Added, // new: kept at the end of entries()
            Duplicate, // an equal entry is already here, at foundEntry()
            Full // new, but there is no room; nothing changed
        };

        /// Keeps `tuple` unless an equal entry (on the key) is already here.
        InsertResult insertIfNew(const Tuple& tuple);

        /// After insertIfNew returned Duplicate: the position of the equal
        /// entry in entries().tuples().
        std::size_t foundEntry() const { return foundEntry_; }

        /// The entries in insertion order. To write them out, use
        /// giveFrameToOutput and popBack on it, then call clearBuckets().
        BoundedTupleBuffer& entries() { return entries_; }

        /// Forgets the bucket lists (after the entries were emptied).
        void clearBuckets();

    private:
        BoundedTupleBuffer entries_;
        std::vector<std::vector<std::size_t>> buckets_;
        std::vector<std::size_t> keyColumns_;
        std::size_t foundEntry_ = 0;
        std::uint64_t& comparisons_;
    };
} // namespace db::exec
