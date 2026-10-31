#pragma once

/// @file
/// describePage (chunk 15): everything the `storage layout` command prints
/// about one page of a table file, as a plain value.
///
/// The library never prints (house rule), so describePage only *collects*:
/// it pins the page once, copies every header field, slot or directory entry
/// (and the decoded rows) into a PageLayout, unpins, and returns it. The
/// struct never points into a buffer-pool frame, so it stays valid after
/// the page is gone. The printer in `apps/` turns it into text or JSON.
///
/// Which kind of page it is comes from the page order of heap_file.h:
/// directory pages are exactly the multiples of kDirStride (682), every
/// other page is a data (slotted) page.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "db/common/value.h"
#include "db/storage/directory_page.h"
#include "db/storage/page.h"
#include "db/storage/slotted_page.h"
#include "db/storage/storage_engine.h"

namespace db::storage
{
    // --- Byte offsets of the header fields, for the printer ------------------
    // Data (slotted) page, §M1.4.
    inline constexpr std::size_t kDataPageIdOffset = 0;    ///< page_id, uint32
    inline constexpr std::size_t kDataSlotCountOffset = 4; ///< slot_count, uint16
    inline constexpr std::size_t kDataFreeEndOffset = 6;   ///< free_end, uint16
    /// Slot i's directory entry starts at kDataSlotsOffset + 4 * i.
    inline constexpr std::size_t kDataSlotsOffset = kPageHeaderSize;
    // Directory page, §M1.5.
    inline constexpr std::size_t kDirPageIdOffset = 0;      ///< page_id, uint32
    inline constexpr std::size_t kDirNextPageOffset = 4;    ///< next_dir_page, uint32
    inline constexpr std::size_t kDirEntryCountOffset = 8;  ///< entry_count, uint16
    /// Entry i starts at kDirEntriesOffset + 6 * i.
    inline constexpr std::size_t kDirEntriesOffset = kDirHeaderSize;
    static_assert(kDataSlotsOffset == 8 && kDirEntriesOffset == 10, "SPEC M1.4 / M1.5 header sizes");

    /// Data page or directory page.
    enum class PageKind
    {
        Data,
        Directory
    };

    /// One slot of a data page.
    struct SlotLayout
    {
        std::uint16_t slot = 0;        ///< slot number i
        std::size_t entryOffset = 0;   ///< where the slot's (offset, length) pair sits: 8 + 4i
        std::uint16_t offset = 0;      ///< record start in the page; 0 = dead slot
        std::uint16_t length = 0;      ///< record length L in bytes; 0 for a dead slot
        bool live = false;             ///< offset != 0
        Tuple values;                  ///< the decoded record (owns its strings); empty if dead
    };

    /// One entry of a directory page.
    struct DirEntryLayout
    {
        std::uint16_t index = 0;       ///< entry number i
        std::size_t entryOffset = 0;   ///< where the entry sits: 10 + 6i
        PageId dataPageId = 0;         ///< the data page it lists
        std::uint16_t freeBytes = 0;   ///< free bytes the directory records for that page
    };

    /// One page, described. `kind` says which half is filled:
    /// - Data: slotCount, freeEnd, freeSpace, slots (one per slot, dead ones too,
    ///   in slot order); nextDirPage/entryCount are 0 and entries is empty.
    /// - Directory: nextDirPage, entryCount, entries (in entry order);
    ///   slotCount/freeEnd/freeSpace are 0 and slots is empty.
    /// A kind enum plus two vectors (not a std::variant) so the printer reads
    /// plain fields with no visitor.
    struct PageLayout
    {
        std::string table;             ///< table name as given
        PageId pageId = 0;             ///< the page_id field read from the page
        PageId filePages = 0;          ///< pages in the table file (all kinds)
        PageKind kind = PageKind::Data;

        // Data page (§M1.4).
        std::uint16_t slotCount = 0;   ///< slots, dead ones included
        std::uint16_t freeEnd = 0;     ///< lowest byte used by record data
        std::uint16_t freeSpace = 0;   ///< freeEnd - (8 + 4 * slotCount)
        std::vector<SlotLayout> slots;

        // Directory page (§M1.5).
        PageId nextDirPage = 0;        ///< kInvalidPageId (0xFFFFFFFF) = none
        std::uint16_t entryCount = 0;
        std::vector<DirEntryLayout> entries;
    };

    /// Describes page `pageId` of table `table`'s file. Pins the page once
    /// through the engine's pool (one hit or miss is counted, like any fetch)
    /// and has released it on return; at most one page's worth of decoded
    /// rows is kept.
    ///
    /// Errors:
    /// - unknown table → db::Error(Name) "Unknown table 'X'.";
    /// - pageId ≥ the file's page count → db::Error(Name)
    ///   "Table 'R' has N pages; there is no page P.";
    /// - a corrupt page → the page views' std::runtime_error.
    PageLayout describePage(StorageEngine& engine, const std::string& table, PageId pageId);
} // namespace db::storage
