#include "db/storage/buffer_pool.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "buffer/replacer.h"

namespace db::storage
{
    namespace
    {
        // How error messages name a page: "page P of file F".
        std::string pageText(FileId fileId, PageId pageId)
        {
            return "page " + std::to_string(pageId) + " of file " + std::to_string(fileId);
        }
    } // namespace

    BufferPool::BufferPool(std::size_t frames, ReplacementPolicy policy, FileRegistry& files)
        : files_(files)
    {
        if (frames == 0)
            throw std::logic_error("BufferPool: needs at least one frame");
        // The only page memory in the whole system. Made once here and never
        // resized, because spans handed out point into it.
        frames_.resize(frames);
        pinCounts_.assign(frames, 0);
        replacer_ = makeReplacer(policy, frames);
    }

    // Written here, not in the header: to delete the replacer, the
    // unique_ptr needs the full Replacer class, which only this file sees.
    BufferPool::~BufferPool() = default;

    std::optional<std::size_t> BufferPool::findFrame(FileId fileId, PageId pageId) const
    {
        const auto found = pageTable_.find({fileId, pageId});
        if (found == pageTable_.end())
            return std::nullopt;
        return found->second;
    }

    void BufferPool::pin(std::size_t frame)
    {
        // Going from 0 to 1 pin means one more frame is in use.
        // That is the only moment the peak can go up.
        if (pinCounts_[frame] == 0)
        {
            ++pinnedFrames_;
            peakPinned_ = std::max(peakPinned_, pinnedFrames_);
        }
        ++pinCounts_[frame];
    }

    void BufferPool::writeBack(std::size_t frame)
    {
        Frame& f = frames_[frame];
        files_.disk(f.fileId).writePage(f.pageId, f.bytes);
        // §M1.6: a write is one call to the disk manager's write.
        ++stats_.writes;
        f.dirty = false;
    }

    BufferPool::TakenFrame BufferPool::takeFrame(FileId fileId, PageId pageId)
    {
        // 1. An empty frame, lowest number first. Filling it throws out no
        //    page, so it is not an eviction. This is why the first B misses
        //    on an empty pool are not evictions.
        for (std::size_t i = 0; i < frames_.size(); ++i)
            if (!frames_[i].inUse)
                return {i, false};

        // 2. No empty frame: ask the replacer. It only picks frames with pin
        //    count 0. Nothing has changed yet, so if this throws, the pool
        //    is exactly as it was.
        const std::optional<FrameId> victim = replacer_->chooseVictim(pinCounts_);
        if (!victim)
            throw std::logic_error("No unpinned frame available for " + pageText(fileId, pageId) + " ("
                + std::to_string(frames_.size()) + " frames, all pinned)");

        Frame& f = frames_[*victim];
        // 3. The victim's bytes are about to be overwritten. If they were
        //    changed, save them first, or the change is lost.
        if (f.dirty)
            writeBack(*victim);
        // 4. The old page is no longer in memory: remove it from the page table.
        pageTable_.erase({f.fileId, f.pageId});
        f.inUse = false; // the caller fills the frame in
        return {*victim, true};
    }

    std::span<std::byte> BufferPool::fetchPage(FileId fileId, PageId pageId)
    {
        // HIT: the page is already in a frame. No disk access.
        if (const auto frame = findFrame(fileId, pageId))
        {
            pin(*frame);
            replacer_->recordHit(*frame);
            ++stats_.hits;
            return frames_[*frame].bytes;
        }

        // MISS. All checks come before the first change, so a failed fetch
        // leaves every frame and counter as it was.
        DiskManager& disk = files_.disk(fileId); // throws for an unknown file
        if (pageId >= disk.numPages())
            throw std::logic_error("BufferPool::fetchPage: " + pageText(fileId, pageId)
                + " does not exist (file has " + std::to_string(disk.numPages()) + " pages)");
        const TakenFrame taken = takeFrame(fileId, pageId);

        // Read the page into the frame and record what the frame now holds.
        Frame& f = frames_[taken.index];
        disk.readPage(pageId, f.bytes);
        ++stats_.reads;
        f.fileId = fileId;
        f.pageId = pageId;
        f.dirty = false;
        f.inUse = true;
        pageTable_[{fileId, pageId}] = taken.index;
        pin(taken.index);
        replacer_->recordLoad(taken.index);

        ++stats_.misses;
        // An eviction is a miss that threw out another page (§M1.6).
        if (taken.replacedPage)
            ++stats_.evictions;
        return f.bytes;
    }

    void BufferPool::unpinPage(FileId fileId, PageId pageId, bool isDirty)
    {
        const auto frame = findFrame(fileId, pageId);
        if (!frame)
            throw std::logic_error("BufferPool::unpinPage: " + pageText(fileId, pageId) + " is not resident");
        // §M1.8: unpinning at pin count 0 is an error. It "catches the most
        // common bug in this module" (an unpin with no matching fetch).
        if (pinCounts_[*frame] == 0)
            throw std::logic_error("BufferPool::unpinPage: " + pageText(fileId, pageId)
                + " is already at pin count 0");

        // Sticky dirty: once changed, a later clean unpin cannot hide it.
        frames_[*frame].dirty = frames_[*frame].dirty || isDirty;
        --pinCounts_[*frame];
        if (pinCounts_[*frame] == 0)
            --pinnedFrames_;
    }

    NewPage BufferPool::newPage(FileId fileId)
    {
        DiskManager& disk = files_.disk(fileId); // throws for an unknown file
        // Get a frame BEFORE growing the file. If no frame is free, this
        // throws and the file stays the same size.
        // numPages() is the number the new page is about to get.
        const TakenFrame taken = takeFrame(fileId, disk.numPages());
        const PageId pageId = disk.allocatePage(); // not counted as a write (D2)

        Frame& f = frames_[taken.index];
        // The frame may still hold an old page's bytes: clear them.
        f.bytes.fill(std::byte{0});
        f.fileId = fileId;
        f.pageId = pageId;
        // §M1.6 "zero filled, pinned, dirty": the caller is about to fill
        // it, and the page reaches disk at its eviction or flush (counted then).
        f.dirty = true;
        f.inUse = true;
        pageTable_[{fileId, pageId}] = taken.index;
        pin(taken.index);
        replacer_->recordLoad(taken.index);

        // No hit, miss, eviction or read: nothing was fetched (chunk 05 Q2).
        // If a changed victim had to be saved, takeFrame already counted
        // that write.
        return {pageId, f.bytes};
    }

    void BufferPool::flushPage(FileId fileId, PageId pageId)
    {
        // Clean or not in memory: nothing to save, and that is not an error.
        if (const auto frame = findFrame(fileId, pageId); frame && frames_[*frame].dirty)
            writeBack(*frame);
    }

    void BufferPool::flushAll()
    {
        // Frame order every time, so the writes happen in the same order on
        // every run. Pinned pages are saved too; nothing is evicted.
        for (std::size_t i = 0; i < frames_.size(); ++i)
            if (frames_[i].inUse && frames_[i].dirty)
                writeBack(i);
    }

    void BufferPool::discardFile(FileId fileId)
    {
        // Pass 1: if any page of the file is still in use, refuse before
        // changing anything.
        for (std::size_t i = 0; i < frames_.size(); ++i)
            if (frames_[i].inUse && frames_[i].fileId == fileId && pinCounts_[i] > 0)
                throw std::logic_error("BufferPool::discardFile: " + pageText(fileId, frames_[i].pageId)
                    + " is still pinned");

        // Pass 2: throw the pages away, without saving them, even if changed.
        // Each frame becomes empty, so reusing it later is not an eviction
        // (chunk 04 decision: forget means "back on the free list").
        for (std::size_t i = 0; i < frames_.size(); ++i)
        {
            Frame& f = frames_[i];
            if (!f.inUse || f.fileId != fileId)
                continue;
            pageTable_.erase({f.fileId, f.pageId});
            f.dirty = false;
            f.inUse = false;
            replacer_->forget(i);
        }
    }

    bool BufferPool::isResident(FileId fileId, PageId pageId) const
    {
        return findFrame(fileId, pageId).has_value();
    }

    IoStats BufferPool::stats() const
    {
        return stats_;
    }

    std::size_t BufferPool::frameCount() const
    {
        return frames_.size();
    }

    std::size_t BufferPool::pinnedFrameCount() const
    {
        return pinnedFrames_;
    }

    std::size_t BufferPool::peakPinnedCount() const
    {
        return peakPinned_;
    }

    void BufferPool::resetPeak()
    {
        peakPinned_ = pinnedFrames_;
    }

} // namespace db::storage
