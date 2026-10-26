#pragma once

/// @file
/// PageGuard: an object that holds one pin on one page and gives it back
/// automatically when the guard goes away.
///
/// The idea is called RAII ("Resource Acquisition Is Initialisation"): you
/// get the resource (the pin) when the object is made, and C++ calls the
/// destructor, which gives it back, on every way out of a block: a normal
/// end, `return`, `break`, or an exception. So a page can never be left
/// pinned by mistake, and never unpinned twice (§M1.8 "Unpinning a page at
/// pin count 0").
///
/// From chunk 07 on, all code reaches pages through guards. Raw
/// fetchPage/unpinPage calls appear only inside this class and in the pool tests.

#include <span>

#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/page.h"

namespace db::storage
{

    /// A guard is either *valid* (it holds one pin) or *empty* (after
    /// release(), after being moved from, or made with PageGuard()).
    ///
    /// It can be moved but not copied. Moving hands the pin over to the new
    /// guard; copying would make two owners of one pin, and both would unpin.
    ///
    /// Calling data(), fileId(), pageId() or markDirty() on an empty guard
    /// is a bug in the caller and throws std::logic_error (chunk 06 open
    /// question, option a).
    ///
    /// The span from data() points straight into the pool's frame. It is
    /// usable only while the guard is valid. After release(), do not touch it.
    class PageGuard
    {
    public:
        /// Pins page (fileId, pageId) through BufferPool::fetchPage (the pool
        /// counts the hit or miss). If fetchPage throws, no guard is made
        /// and nothing stays pinned.
        static PageGuard fetch(BufferPool& pool, FileId fileId, PageId pageId);

        /// Adds a new all-zero page at the end of the file through
        /// BufferPool::newPage. The pool already marks it dirty.
        /// pageId() tells you the new page's number.
        static PageGuard newPage(BufferPool& pool, FileId fileId);

        /// An empty guard, for a variable that gets a page later (chunk 09's
        /// scan keeps one guard and moves it from page to page).
        PageGuard() = default;

        /// The "rule of five": because this class has its own destructor and
        /// moves, all five special functions are spelled out here.
        /// Copying is not allowed: two copies would each unpin, releasing
        /// one pin twice.
        PageGuard(const PageGuard&) = delete;
        PageGuard& operator=(const PageGuard&) = delete;
        /// Move: the new guard takes over `other`'s pin and `other` becomes
        /// empty, so `other`'s destructor does nothing.
        PageGuard(PageGuard&& other) noexcept;
        /// Move assignment: first gives back this guard's own pin (if it has
        /// one), then takes over `other`'s. Assigning a guard to itself does nothing.
        PageGuard& operator=(PageGuard&& other) noexcept;
        /// Unpins the page when the guard goes away (it calls release()).
        /// Marked noexcept, meaning "this never throws". C++ stops the whole
        /// program if a destructor throws while another exception is already
        /// being handled, so destructors must not throw.
        /// A valid guard always holds one real pin, so the unpin cannot fail.
        /// If it ever does, the pool has a bug, and stopping the program
        /// makes that bug obvious.
        ~PageGuard() noexcept;

        /// The page's bytes, which you may change. Call markDirty() if you do.
        std::span<std::byte> data();
        /// The page's bytes, read only.
        std::span<const std::byte> data() const;

        FileId fileId() const;
        PageId pageId() const;

        /// Says "I changed this page". When the guard unpins, it tells the
        /// pool the page is dirty, so the pool saves it to disk (one WRITE)
        /// when it is evicted or flushed. Without this, the change is lost.
        void markDirty();

        /// Unpins the page now (passing on the dirty flag) and makes the
        /// guard empty. On an empty guard it does nothing, so calling it
        /// twice is safe.
        void release();

        /// True while the guard holds a pin.
        bool valid() const;

    private:
        PageGuard(BufferPool& pool, FileId fileId, PageId pageId, std::span<std::byte> bytes);

        /// Throws std::logic_error, naming the function `what`, if the guard is empty.
        void requireValid(const char* what) const;

        /// The pool that owns the pin, or nullptr for an empty guard.
        /// A pointer, not a reference: a reference can never be pointed at
        /// another pool, and move assignment needs to do exactly that.
        BufferPool* pool_ = nullptr;
        FileId fileId_ = 0;
        PageId pageId_ = kInvalidPageId;
        std::span<std::byte> bytes_;
        /// true once markDirty() was called.
        bool dirty_ = false;
    };

} // namespace db::storage
