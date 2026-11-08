#include "db/exec/tuple_buffer.h"

#include <cstddef>
#include <utility>

#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/page.h"
#include "db/storage/record_codec.h"
#include "db/storage/slotted_page.h"

namespace db::exec
{
    namespace
    {
        // Bytes a page offers for records and slots: 4096 - 8 = 4088.
        const std::size_t kUsablePageBytes = storage::kPageSize - storage::kPageHeaderSize;

        // Pages needed for `bytes` bytes of records and slots, if they could
        // be packed with no gap: ceil(bytes / 4088). Used only while the
        // buffer is being emptied, to decide when a frame can be given back.
        std::size_t pagesFor(std::size_t bytes)
        {
            return (bytes + kUsablePageBytes - 1) / kUsablePageBytes;
        }
    } // namespace

    BoundedTupleBuffer::BoundedTupleBuffer(storage::BufferPool& pool, const Schema& schema, std::size_t capacityPages,
                                           bool chargeLargestSize)
        : pool_(pool), schema_(schema), capacityPages_(capacityPages), chargeLargestSize_(chargeLargestSize)
    {
    }

    BoundedTupleBuffer::~BoundedTupleBuffer()
    {
        pool_.releaseFrames(pagesUsed_);
    }

    std::size_t BoundedTupleBuffer::bytesOnPage(const Tuple& tuple) const
    {
        if (!chargeLargestSize_)
            return storage::encodedLength(tuple, schema_) + storage::kSlotSize;

        // The largest record of the schema, as the record codec lays it out:
        // INT 4 bytes, REAL 8, VARCHAR(n) a 2-byte length + n bytes.
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < schema_.size(); ++i)
        {
            const Column& column = schema_.column(i);
            if (column.type == Type::Int)
                bytes += 4;
            else if (column.type == Type::Real)
                bytes += 8;
            else
                bytes += 2 + static_cast<std::size_t>(column.varcharLength);
        }
        return bytes + storage::kSlotSize;
    }

    bool BoundedTupleBuffer::tryAdd(Tuple tuple)
    {
        // Like a real page: the tuple goes on the last page if it fits there,
        // otherwise it starts a new page.
        const std::size_t bytes = bytesOnPage(tuple);
        const bool fitsOnLastPage = pagesUsed_ > 0 && bytesOnLastPage_ + bytes <= kUsablePageBytes;
        const std::size_t newPages = fitsOnLastPage ? pagesUsed_ : pagesUsed_ + 1;
        if (newPages > capacityPages_)
            return false;

        // Reserve before keeping the tuple: if the pool has no frame left it
        // throws, and the buffer is unchanged.
        if (!fitsOnLastPage)
            pool_.reserveFrames(1);
        tuples_.push_back(std::move(tuple));
        totalBytes_ += bytes;
        bytesOnLastPage_ = fitsOnLastPage ? bytesOnLastPage_ + bytes : bytes;
        pagesUsed_ = newPages;
        return true;
    }

    void BoundedTupleBuffer::popBack()
    {
        totalBytes_ -= bytesOnPage(tuples_.back());
        // pop_back() returns void and removes the last element from the container, destroying it
        // and reducing the vector's size() by 1 in O(1) constant time.
        tuples_.pop_back();

        // Frames still needed by the remaining tuples' bytes. After
        // giveFrameToOutput, one of them is the output page, so keep one fewer.
        std::size_t keep = pagesFor(totalBytes_);
        if (frameGiven_ && keep > 0)
            --keep;
        // Give back every frame the remaining tuples no longer need. Pages
        // that filled with gaps can make this more than one frame the first time.
        pool_.releaseFrames(pagesUsed_ - keep);
        pagesUsed_ = keep;

        if (tuples_.empty())
        {
            // Empty: start counting from scratch.
            bytesOnLastPage_ = 0;
            frameGiven_ = false;
        }
        else
        {
            // Which page each remaining tuple would sit on is no longer known,
            // so call the last page full: a new tuple then starts a new page.
            bytesOnLastPage_ = kUsablePageBytes;
        }
    }

    void BoundedTupleBuffer::giveFrameToOutput()
    {
        // The caller only calls this on a buffer that holds tuples, so at
        // least one frame is reserved.
        pool_.releaseFrames(1);
        --pagesUsed_;
        frameGiven_ = true;
    }

    void BoundedTupleBuffer::clear()
    {
        pool_.releaseFrames(pagesUsed_);
        tuples_.clear();
        totalBytes_ = 0;
        bytesOnLastPage_ = 0;
        pagesUsed_ = 0;
        frameGiven_ = false;
    }

    std::size_t BoundedTupleBuffer::pagesUsed() const
    {
        return pagesUsed_;
    }

    std::size_t BoundedTupleBuffer::size() const
    {
        return tuples_.size();
    }

    std::vector<Tuple>& BoundedTupleBuffer::tuples()
    {
        return tuples_;
    }
} // namespace db::exec
