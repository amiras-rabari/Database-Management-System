#pragma once

/// @file
/// The disk manager: reads and writes the 4096-byte pages of one file and
/// counts every call (SPEC §M1.1, §M1.6). The only class that touches a
/// table file's bytes.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>

#include "db/storage/page.h"

namespace db::storage
{

    /// One file of pages. The file has no header: its page count is its
    /// size divided by kPageSize. Never allocates a page buffer itself,
    /// callers pass theirs in.
    ///
    /// The file is opened on the first page call and stays open until
    /// closeFile() (or destruction). FileRegistry closes the least recently
    /// used files so that only a bounded number are open at once: a hash
    /// join at B = 256 keeps 2 x 255 partition files, more than the about
    /// 512 open files Windows allows (exec spec audit, user's choice (b)).
    ///
    /// Counting (§M1.6): one readPage call = one read, one writePage call =
    /// one write. allocatePage and opening the file count nothing.
    class DiskManager
    {
    public:
        /// Opens an existing file. Throws db::Error(Name) if it does not
        /// exist, std::runtime_error if its size is not a multiple of
        /// kPageSize or it cannot be opened. Errors name the file.
        static DiskManager open(const std::filesystem::path& path);
        /// Creates a new empty file (0 pages). Throws db::Error(Name) if the
        /// file already exists (it is never truncated).
        static DiskManager create(const std::filesystem::path& path);

        /// The file this object reads and writes.
        const std::filesystem::path& path() const;
        /// Number of pages in the file. No I/O, nothing counted.
        PageId numPages() const;

        /// Copies page `pageId` into `out`, which must be exactly kPageSize
        /// bytes. Counts one read. Throws std::logic_error if `pageId` is not
        /// below numPages() or `out` has the wrong size.
        void readPage(PageId pageId, std::span<std::byte> out);
        /// Writes `bytes` (exactly kPageSize) as page `pageId`. Counts one
        /// write. Throws std::logic_error if the page was never allocated.
        void writePage(PageId pageId, std::span<const std::byte> bytes);
        /// Appends one zero-filled page to the file and returns its id (the
        /// old numPages()). Not counted as a write (M1 decision D2).
        PageId allocatePage();

        /// Closes the file's stream, if open. The next page call opens it
        /// again. Nothing is counted.
        void closeFile();
        /// True while the file's stream is open.
        bool isOpen() const;

        /// readPage calls made through this object.
        std::uint64_t readCount() const;
        /// writePage calls made through this object.
        std::uint64_t writeCount() const;

    private:
        DiskManager(std::filesystem::path path, PageId numPages);

        /// The open stream, opened first if it is closed. Throws
        /// std::runtime_error naming the file if it cannot be opened.
        std::fstream& stream();

        std::filesystem::path path_;
        /// Empty while the file is closed.
        std::optional<std::fstream> file_;
        PageId numPages_;
        std::uint64_t reads_ = 0;
        std::uint64_t writes_ = 0;
    };

} // namespace db::storage
