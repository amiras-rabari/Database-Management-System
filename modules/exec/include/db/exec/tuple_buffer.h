#pragma once

#include <cstddef>
#include <vector>

#include "db/common/schema.h"
#include "db/storage/buffer_pool.h"

namespace db::exec
{
    /// A list of tuples whose size is counted in pages. This is the only way
    /// an operator may keep many tuples in memory (sort runs, hash tables,
    /// the nested loop block): SPEC §M2.1 counts tuples in a C++ container
    /// against the B frames.
    ///
    /// A tuple costs what it would take on a slotted page: its record bytes
    /// (storage encodedLength) plus a 4-byte slot. A page offers
    /// 4096 - 8 = 4088 bytes. Pages fill like real pages (chunk 07, changed
    /// from chunk 03's byte total): a tuple goes on the last page if it fits
    /// there, otherwise it starts a new page. Example: 13-byte records cost
    /// 17 bytes, so a page holds 240 of them (8 bytes wasted) and 3 pages
    /// hold 720, exactly what a run of 3 pages holds on disk. A byte total
    /// would allow 721 (721 * 17 <= 3 * 4088), which needs 4 pages on disk.
    ///
    /// Each page the buffer uses is a frame reserved in the buffer pool
    /// (D2 (b)), taken when the buffer grows onto a new page and given back
    /// by popBack, clear and the destructor. The pool's peak counter then
    /// includes this memory.
    ///
    /// Not copyable: two copies would give the same frames back twice.
    /// The pool and the schema must live longer than the buffer.
    class BoundedTupleBuffer
    {
    public:
        /// An empty buffer that may use up to `capacityPages` pages.
        /// Reserves nothing yet.
        ///
        /// With `chargeLargestSize`, every tuple costs the most bytes a
        /// tuple of `schema` can take (each VARCHAR(n) as n bytes), whatever
        /// its values. Then a kept tuple may be changed in place through
        /// tuples() without changing the count. Hash group_by needs this:
        /// a group's MAX of a VARCHAR can grow from "ab" to "abcdef"
        /// (chunk 10).
        BoundedTupleBuffer(storage::BufferPool& pool, const Schema& schema, std::size_t capacityPages,
                           bool chargeLargestSize = false);
        ~BoundedTupleBuffer();

        /// disable copy constructor and diables copy assignments
        BoundedTupleBuffer(const BoundedTupleBuffer&) = delete;
        BoundedTupleBuffer& operator=(const BoundedTupleBuffer&) = delete;

        /// Keeps `tuple` if the buffer still fits in its capacity with it,
        /// reserving one more frame if it starts a new page. Returns false
        /// and changes nothing if it would not fit (the caller then writes
        /// its tuples out, e.g. a sort run). A pool with no frame to reserve
        /// throws std::logic_error (the plan used more than divideFrames gave).
        bool tryAdd(Tuple tuple);

        /// Removes the last tuple. Gives a frame back when the remaining
        /// tuples' bytes fit in one page fewer. The buffer must not be empty.
        /// Add again only once the buffer is empty: until then a new tuple
        /// always starts a new page (the page layout after removals is not
        /// tracked, so the buffer counts on the safe side).
        void popBack();

        /// For writing the tuples out as a run (chunk 07 Q3 (a)): gives one
        /// frame back to the pool, for the page the tuples are copied onto.
        /// That page is one of the buffer's frames: each tuple leaves the
        /// buffer (popBack) as it goes onto the page, so the data in memory
        /// never grows. From now until the buffer is empty, popBack keeps one
        /// frame fewer than the remaining tuples need. Call it once, on a
        /// buffer that is not empty, then remove every tuple with popBack.
        void giveFrameToOutput();

        /// Removes every tuple and gives every frame back.
        void clear();

        /// Pages (= reserved frames) in use now.
        std::size_t pagesUsed() const;
        /// Number of tuples kept.
        std::size_t size() const;

        /// The kept tuples, in the order added. The caller may reorder them
        /// (sort a run) but must add and remove only through this class.
        std::vector<Tuple>& tuples();

    private:
        /// What `tuple` costs on a page: record bytes + 4-byte slot (the
        /// largest record bytes with chargeLargestSize).
        std::size_t bytesOnPage(const Tuple& tuple) const;

        storage::BufferPool& pool_;
        const Schema& schema_;
        std::size_t capacityPages_;
        bool chargeLargestSize_;
        std::vector<Tuple> tuples_;
        /// Sum of bytesOnPage over tuples_.
        std::size_t totalBytes_ = 0;
        /// Bytes used on the last page, while the buffer is filling.
        std::size_t bytesOnLastPage_ = 0;
        /// Frames reserved in the pool now.
        std::size_t pagesUsed_ = 0;
        /// True between giveFrameToOutput and the buffer becoming empty.
        bool frameGiven_ = false;
    };
} // namespace db::exec
