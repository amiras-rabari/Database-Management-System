#include "db/storage/heap_scan.h"

#include <utility>

#include "db/storage/directory_page.h"
#include "db/storage/page.h"
#include "db/storage/record_codec.h"
#include "db/storage/slotted_page.h"

namespace db::storage
{
    HeapScan::HeapScan(BufferPool& pool, FileId fileId, Schema schema)
        : pool_(&pool), fileId_(fileId), schema_(std::move(schema))
    {
        // Lazy start: nothing is pinned or read here.
    }

    HeapScan::HeapScan(HeapScan&& other) noexcept
        : pool_(other.pool_), fileId_(other.fileId_), schema_(std::move(other.schema_)),
          // std::exchange returns other's state and sets other's to Finished
          // in one step, so the moved-from scan can never restart.
          state_(std::exchange(other.state_, State::Finished)), dirGuard_(std::move(other.dirGuard_)),
          nextEntry_(other.nextEntry_), dataGuard_(std::move(other.dataGuard_)), nextSlot_(other.nextSlot_)
    {
    }

    HeapScan& HeapScan::operator=(HeapScan&& other) noexcept
    {
        if (this != &other)
        {
            // Guard move assignment releases this scan's own pin first, then
            // takes over other's.
            dataGuard_ = std::move(other.dataGuard_);
            dirGuard_ = std::move(other.dirGuard_);
            pool_ = other.pool_;
            fileId_ = other.fileId_;
            schema_ = std::move(other.schema_);
            state_ = std::exchange(other.state_, State::Finished);
            nextEntry_ = other.nextEntry_;
            nextSlot_ = other.nextSlot_;
        }
        return *this;
    }

    bool HeapScan::next(Rid& rid, Tuple& tuple)
    {
        if (state_ == State::Finished)
            return false;
        try
        {
            return advance(rid, tuple);
        }
        catch (...)
        {
            // A failed fetch (e.g. no free frame) or a corrupt page: drop the
            // pins so nothing leaks, then let the caller see the error.
            close();
            throw;
        }
    }

    bool HeapScan::advance(Rid& rid, Tuple& tuple)
    {
        if (state_ == State::NotStarted)
        {
            // First call: start at directory page 0 (§M1.5).
            dirGuard_ = PageGuard::fetch(*pool_, fileId_, 0);
            nextEntry_ = 0;
            state_ = State::Running;
        }

        while (true)
        {
            // 1. "for each live slot: yield" on the current data page.
            if (dataGuard_.valid())
            {
                const SlottedPage page(dataGuard_.data());
                while (nextSlot_ < page.slotCount())
                {
                    const std::uint16_t slot = nextSlot_++;
                    if (!page.isLive(slot))
                        continue; // deleted record: skip it
                    rid = Rid{dataGuard_.pageId(), slot};
                    // decodeRecord copies the bytes, so the tuple stays valid
                    // after this page is unpinned.
                    tuple = decodeRecord(page.get(slot), schema_);
                    return true;
                }
                // "unpin data page before moving on": released here, before
                // the next data page is fetched, so at most one is pinned.
                dataGuard_.release();
            }

            // 2. "for each entry": the next data page listed on this directory page.
            const DirectoryPage dir(dirGuard_.data());
            if (nextEntry_ < dir.entryCount())
            {
                dataGuard_ = PageGuard::fetch(*pool_, fileId_, dir.entry(nextEntry_).dataPageId);
                ++nextEntry_;
                nextSlot_ = 0;
                continue;
            }

            // 3. "for each directory page": this one is done. Release it
            //    before fetching the next, so at most one is pinned.
            const PageId nextDir = dir.nextDirPage();
            dirGuard_.release();
            if (nextDir == kInvalidPageId)
            {
                state_ = State::Finished;
                return false;
            }
            dirGuard_ = PageGuard::fetch(*pool_, fileId_, nextDir);
            nextEntry_ = 0;
        }
    }

    void HeapScan::close()
    {
        dataGuard_.release();
        dirGuard_.release();
        state_ = State::Finished;
    }
} // namespace db::storage
