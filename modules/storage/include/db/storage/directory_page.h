#pragma once

/// @file
/// The directory page of a heap file (§M1.5): a list of the table's data
/// pages and how many bytes each one still has free.
///
/// Why it exists: to insert a row, we need a data page with enough room.
/// Instead of opening every data page to check, we read this list and take
/// the first page that fits ("first fit").
///
/// Page 0 of every table file is the first directory page. One directory
/// page holds 681 entries; when it is full, a new directory page is added
/// and linked through next_dir_page.
///
/// ```
/// offset    size  field
///      0       4  page_id         uint32
///      4       4  next_dir_page   uint32  (0xFFFFFFFF = no next page)
///      8       2  entry_count     uint16
///     10 + 6i  6  entry i         uint32 data_page_id, uint16 free_bytes
/// ```
/// Entries are only added at the end or updated, never removed or moved.
/// Their order is the order a scan visits the table's pages (chunk 09).

#include <cstddef>
#include <cstdint>
#include <span>

#include "db/storage/page.h"

namespace db::storage
{

    /// Bytes before the first entry: page_id (4) + next_dir_page (4) + entry_count (2) = 10.
    inline constexpr std::size_t kDirHeaderSize = 10;
    /// Bytes per entry: data_page_id (4) + free_bytes (2) = 6.
    inline constexpr std::size_t kDirEntrySize = 6;
    /// How many entries fit on one page: (4096 - 10) / 6 = 681 (§M1.5).
    /// Computed from the layout, so it can never disagree with it.
    /// Check: the last entry (number 680) ends at 10 + 6 * 681 = 4096,
    /// exactly the end of the page.
    inline constexpr std::size_t kDirEntriesPerPage = (kPageSize - kDirHeaderSize) / kDirEntrySize;
    static_assert(kDirEntriesPerPage == 681, "SPEC M1.5: 681 entries per directory page");

    /// One directory entry: a data page and its free bytes.
    /// free_bytes is at most 4088 (an empty slotted page has 4096 - 8 free),
    /// so 16 bits (up to 65535) are enough.
    struct DirectoryEntry
    {
        PageId dataPageId = kInvalidPageId;
        std::uint16_t freeBytes = 0;

        bool operator==(const DirectoryEntry&) const = default;
    };

    /// A view over exactly 4096 bytes, like SlottedPage. It does not own or
    /// copy the bytes and remembers nothing itself: every read and write
    /// goes straight to the bytes, at the §M1.5 offsets.
    /// Methods that only read are const.
    ///
    /// Errors: an entry number >= entryCount(), or addEntry on a full page,
    /// is a bug in the caller (std::logic_error). A page whose entry_count
    /// is above 681 is corrupt (std::runtime_error).
    class DirectoryPage
    {
    public:
        /// Views `bytes`, which must be exactly 4096 bytes long (else
        /// std::logic_error). Does not read or change them yet.
        explicit DirectoryPage(std::span<std::byte> bytes);

        /// Sets the page up as an empty directory page: page_id = `pageId`,
        /// no next page (0xFFFFFFFF), 0 entries. The bytes after the header
        /// are left as they are.
        void init(PageId pageId);

        /// The page_id field (bytes 0..3).
        PageId pageId() const;
        /// The next directory page, or kInvalidPageId if this is the last one (bytes 4..7).
        PageId nextDirPage() const;
        /// Links this page to the next directory page (kInvalidPageId = none).
        void setNextDirPage(PageId next);

        /// How many entries the page holds (bytes 8..9). Throws
        /// std::runtime_error if the page claims more than 681.
        std::uint16_t entryCount() const;
        /// True if no more entries fit (entryCount() == 681).
        bool isFull() const;

        /// Entry number `index`. Throws std::logic_error if index >= entryCount().
        DirectoryEntry entry(std::uint16_t index) const;
        /// Adds an entry after the last one, increases the count by one and
        /// returns the new entry's number. Throws std::logic_error if the
        /// page is full.
        std::uint16_t addEntry(PageId dataPageId, std::uint16_t freeBytes);
        /// Changes entry `index`'s free_bytes (its data page stays the same).
        /// Throws std::logic_error if index >= entryCount().
        void setFreeBytes(std::uint16_t index, std::uint16_t freeBytes);

    private:
        std::span<std::byte> bytes_;
    };

} // namespace db::storage
