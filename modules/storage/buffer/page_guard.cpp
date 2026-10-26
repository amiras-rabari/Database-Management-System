#include "db/storage/page_guard.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace db::storage
{

    PageGuard PageGuard::fetch(BufferPool& pool, FileId fileId, PageId pageId)
    {
        // fetchPage pins the page. If it throws, we never get to make the
        // guard, and there is no pin to give back.
        const std::span<std::byte> bytes = pool.fetchPage(fileId, pageId);
        return PageGuard(pool, fileId, pageId, bytes);
    }

    PageGuard PageGuard::newPage(BufferPool& pool, FileId fileId)
    {
        const NewPage created = pool.newPage(fileId);
        // The pool already marked the new page dirty. The guard's own dirty
        // flag only records changes the caller reports with markDirty().
        return PageGuard(pool, fileId, created.pageId, created.bytes);
    }

    PageGuard::PageGuard(BufferPool& pool, FileId fileId, PageId pageId, std::span<std::byte> bytes)
        : pool_(&pool), fileId_(fileId), pageId_(pageId), bytes_(bytes)
    {
    }

    // std::exchange(x, v) returns x's old value and sets x to v in one step.
    // Here it copies each field from `other` and empties `other` at once.
    PageGuard::PageGuard(PageGuard&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)),
          fileId_(other.fileId_),
          pageId_(std::exchange(other.pageId_, kInvalidPageId)),
          bytes_(std::exchange(other.bytes_, {})),
          dirty_(std::exchange(other.dirty_, false))
    {
        // other.pool_ is now nullptr, so other's destructor will not unpin.
        // The one pin is given back exactly once, by this guard.
    }

    PageGuard& PageGuard::operator=(PageGuard&& other) noexcept
    {
        if (this != &other)
        {
            // Give back the page we hold now. Skipping this would leave
            // that page pinned forever.
            release();
            // Then take over other's pin and empty other (as in the move above).
            pool_ = std::exchange(other.pool_, nullptr);
            fileId_ = other.fileId_;
            pageId_ = std::exchange(other.pageId_, kInvalidPageId);
            bytes_ = std::exchange(other.bytes_, {});
            dirty_ = std::exchange(other.dirty_, false);
        }
        return *this;
    }

    PageGuard::~PageGuard() noexcept
    {
        release();
    }

    void PageGuard::requireValid(const char* what) const
    {
        if (pool_ == nullptr)
            throw std::logic_error(std::string("PageGuard::") + what + ": the guard holds no page");
    }

    std::span<std::byte> PageGuard::data()
    {
        requireValid("data");
        return bytes_;
    }

    std::span<const std::byte> PageGuard::data() const
    {
        requireValid("data");
        return bytes_;
    }

    FileId PageGuard::fileId() const
    {
        requireValid("fileId");
        return fileId_;
    }

    PageId PageGuard::pageId() const
    {
        requireValid("pageId");
        return pageId_;
    }

    void PageGuard::markDirty()
    {
        requireValid("markDirty");
        dirty_ = true;
    }

    void PageGuard::release()
    {
        if (pool_ == nullptr)
            return; // already empty, so a second release does nothing
        // Make the guard empty BEFORE unpinning. Then, even if unpinPage
        // threw, this guard could never try to unpin the same page again.
        BufferPool* pool = std::exchange(pool_, nullptr);
        bytes_ = {};
        pool->unpinPage(fileId_, pageId_, dirty_);
        pageId_ = kInvalidPageId;
        dirty_ = false;
    }

    bool PageGuard::valid() const
    {
        return pool_ != nullptr;
    }

} // namespace db::storage
