#include "db/storage/heap_scan.h"

#include <utility>

#include "db/storage/directory_page.h"
#include "db/storage/page.h"
#include "db/storage/record_codec.h"
#include "db/storage/slotted_page.h"

// How a scan walks a table file:
//
//   directory page 0 ──next──> directory page 682 ──next──> none
//     entry 0 -> data page 1        entry 0 -> data page 683
//     entry 1 -> data page 2        ...
//     ...
//
// The scan remembers where it is with four members:
//   dirGuard_   pins the directory page it is reading now
//   nextEntry_  which entry of that directory page to visit next
//   dataGuard_  pins the data page it is reading now (empty between pages)
//   nextSlot_   which slot of that data page to look at next
// Each call to next() continues from there and stops at the next live row.
//
// The guards are members of the scan, not local variables, so they do NOT
// unpin when next() returns: the pages stay pinned between calls, which is
// what lets the next call continue on the same page. When the scan is done
// with a page, it must call release() itself.

namespace db::storage
{
    HeapScan::HeapScan(BufferPool& pool, FileId fileId, Schema schema)
        : pool_(&pool), fileId_(fileId), schema_(std::move(schema))
    {
        // Nothing is pinned or read here. The first next() pins directory
        // page 0. So a scan that is made but never used costs no I/O.
    }

    // Move constructor: the new scan takes over everything `other` had (its
    // two pins and its position), and `other` is marked Finished so that
    // calling next() on it just returns false.
    //   - The guards hand over their pins by themselves when moved (and the
    //     moved-from guards become empty).
    //   - std::exchange(other.state_, State::Finished) does two things: it
    //     returns other's state (we copy it) and sets other's state to Finished.
    //   - nextEntry_ and nextSlot_ are plain numbers, so they are just copied.
    HeapScan::HeapScan(HeapScan&& other) noexcept
        : pool_(other.pool_), fileId_(other.fileId_), schema_(std::move(other.schema_)),
          state_(std::exchange(other.state_, State::Finished)), dirGuard_(std::move(other.dirGuard_)),
          nextEntry_(other.nextEntry_), dataGuard_(std::move(other.dataGuard_)), nextSlot_(other.nextSlot_)
    {
    }

    // Move assignment (scanA = std::move(scanB)): scanA stops its own scan,
    // then takes over scanB's pins and position; scanB is marked Finished.
    HeapScan& HeapScan::operator=(HeapScan&& other) noexcept
    {
        if (this != &other) // scanA = std::move(scanA) must change nothing
        {
            // Assigning a guard first unpins the page that guard held, then
            // takes over the other guard's pin. So these two lines give back
            // this scan's own pages and take over other's.
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
        // A finished scan (done, closed, or moved away) never reads again.
        if (state_ == State::Finished)
            return false;
        try
        {
            return advance(rid, tuple);
        }
        catch (...)
        {
            // Something failed inside advance(), for example the pool had no
            // free frame for the next page. close() unpins whatever this scan
            // still holds, so no page stays pinned by mistake. Then `throw;`
            // passes the same error on to the caller.
            close();
            throw;
        }
    }

    // Finds the next live row, starting from where the last call stopped.
    // It loops through three steps until it finds a row or runs out of pages:
    //   1. look for a live slot on the current data page
    //   2. if there is none, go to the next data page on this directory page
    //   3. if this directory page has no more entries, go to the next directory page
    bool HeapScan::advance(Rid& rid, Tuple& tuple)
    {
        if (state_ == State::NotStarted)
        {
            // First call ever: pin directory page 0 (the first directory page
            // of every table, §M1.5) and start at its entry 0.
            dirGuard_ = PageGuard::fetch(*pool_, fileId_, 0);
            nextEntry_ = 0;
            state_ = State::Running;
        }

        while (true)
        {
            // Step 1: look for a live slot on the data page we hold (if any).
            // dataGuard_ is empty on the first pass and right after we
            // finished a page; then we skip straight to step 2.
            if (dataGuard_.valid())
            {
                const SlottedPage page(dataGuard_.data());
                // Look at slots nextSlot_, nextSlot_ + 1, ... up to the last slot.
                while (nextSlot_ < page.slotCount())
                {
                    // Take this slot's number, and move nextSlot_ on by one,
                    // so the next call starts at the slot after it.
                    const std::uint16_t slot = nextSlot_++;
                    if (!page.isLive(slot))
                        continue; // this row was deleted: skip it
                    // Found a row. Give back its rid and its values.
                    rid = Rid{dataGuard_.pageId(), slot};
                    // decodeRecord copies the bytes out of the page into the
                    // tuple, so the tuple is still fine after the page is unpinned.
                    tuple = decodeRecord(page.get(slot), schema_);
                    // Return now. dataGuard_ keeps the page pinned, and
                    // nextSlot_ says where to continue on the next call.
                    return true;
                }
                // No live slot left on this page. Unpin it NOW, before step 2
                // pins the next data page. If we only pinned the next page and
                // let the assignment unpin this one, both data pages would be
                // pinned for a moment (3 pins with the directory page), and
                // the scan may hold at most 2 (§M1.5).
                dataGuard_.release();
            }

            // Step 2: go to the next data page listed on this directory page.
            const DirectoryPage dir(dirGuard_.data());
            if (nextEntry_ < dir.entryCount())
            {
                // Pin the data page that entry nextEntry_ points to, move
                // nextEntry_ on by one, and start that page at slot 0.
                dataGuard_ = PageGuard::fetch(*pool_, fileId_, dir.entry(nextEntry_).dataPageId);
                ++nextEntry_;
                nextSlot_ = 0;
                continue; // back to step 1, to read the new page's slots
            }

            // Step 3: every entry of this directory page has been visited.
            // Read where the next directory page is BEFORE unpinning this
            // one (after release() we may not touch its bytes any more).
            const PageId nextDir = dir.nextDirPage();
            // Unpin this directory page now, before pinning the next one, for
            // the same reason as in step 1: at most one directory page pinned.
            dirGuard_.release();
            if (nextDir == kInvalidPageId)
            {
                // That was the last directory page: the whole table has been
                // read. Nothing is pinned any more.
                state_ = State::Finished;
                return false;
            }
            // Pin the next directory page and start at its entry 0, then go
            // round the loop again (step 1 is skipped: no data page is held).
            dirGuard_ = PageGuard::fetch(*pool_, fileId_, nextDir);
            nextEntry_ = 0;
        }
    }

    void HeapScan::close()
    {
        // Unpin both pages now (release() on an empty guard does nothing, so
        // calling close() twice is fine), and mark the scan Finished so that
        // next() returns false from now on.
        dataGuard_.release();
        dirGuard_.release();
        state_ = State::Finished;
    }
} // namespace db::storage
