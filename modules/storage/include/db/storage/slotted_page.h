#pragma once

/// @file
/// The slotted page of §M1.4: a view over one 4096-byte page holding
/// variable-length records. The header and the slot directory grow forward
/// from byte 0; record data grows backward from the end of the page.
///
/// ```
/// offset  size  field
///      0     4  page_id      uint32
///      4     2  slot_count   uint16  (includes dead slots)
///      6     2  free_end     uint16  (lowest byte offset used by record data)
///      8     4  slot 0       uint16 offset, uint16 length
///     12     4  slot 1       ...
/// ```
/// A slot with offset 0 is dead. Slot numbers are never reused or renumbered,
/// so a rid (page id, slot) stays valid. No compaction : a
/// delete frees no usable space.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "db/storage/page.h"

namespace db::storage
{
    /// Bytes before the slot directory: page_id (4) + slot_count (2) + free_end (2).
    inline constexpr std::size_t kPageHeaderSize = 8;
    /// Bytes per slot directory entry: uint16 offset + uint16 length.
    inline constexpr std::size_t kSlotSize = 4;

    /// The record length limit as §M1.4 states it. An empty page has
    /// 4096 - 8 = 4088 bytes free and an insert needs L + 4, so the largest
    /// record that really fits is 4084 : insert returns no
    /// space for 4085..4088 and the heap file reports "record too long".
    inline constexpr std::uint32_t kMaxRecordLength = 4088;

    /// Records of fixed length `recordLength` that fit on one empty page:
    /// floor(4088 / (L + 4)) (§M1.4 capacity). 240 for L = 13.
    constexpr std::uint32_t recordsPerPage(std::uint32_t recordLength)
    {
        return static_cast<std::uint32_t>((kPageSize - kPageHeaderSize) / (recordLength + kSlotSize));
    }

    /// One slot directory entry. `offset == 0` means the slot is dead.
    struct SlotEntry
    {
        std::uint16_t offset = 0;
        std::uint16_t length = 0;

        /// overriding the equals method to compare 2 structs  entries offset and len and uses default strategy
        /// which compares val of both fields here and return if both matches
        bool operator==(const SlotEntry&) const = default;
    };

    /// A non-owning view over exactly kPageSize bytes (a test's array or a
    /// buffer-pool frame). Copying the view copies only the pointer, never the
    /// page. Every field is read from and written to the bytes themselves at
    /// the §M1.4 offsets; nothing is cached in the object, so two views of
    /// the same frame always agree.
    ///
    /// Read methods are const: code that must not change a page takes a
    /// `const SlottedPage&`.
    ///
    /// Errors: a dead or missing slot in get/remove is db::Error(Name) naming
    /// the rid as `(page_id, slot)`; slot(i) out of range is std::logic_error
    /// (caller bug); header or slot bytes that contradict each other (a corrupt
    /// page) are std::runtime_error.
    class SlottedPage
    {
    public:
        /// Views `bytes`, which must be exactly kPageSize long (else
        /// std::logic_error). Does not read or change them.
        explicit SlottedPage(std::span<std::byte> bytes);

        /// Formats the page as empty: page_id = `pageId`, slot_count = 0,
        /// free_end = 4096. Bytes after the header are left as they are.
        void init(PageId pageId);

        /// The page_id field (bytes 0..3).
        PageId pageId() const;
        /// Number of slots, dead ones included (bytes 4..5).
        std::uint16_t slotCount() const;
        /// Lowest byte offset used by record data; 4096 on an empty page (bytes 6..7).
        std::uint16_t freeEnd() const;
        /// free_end - (8 + 4 * slot_count): bytes between the slot directory
        /// and the record data. 4088 on an empty page. Throws
        /// std::runtime_error if free_end is below the end of the directory.
        std::uint16_t freeSpace() const;

        /// Slot directory entry `slotNo`, dead or live. Throws
        /// std::logic_error if `slotNo >= slotCount()`.
        SlotEntry slot(std::uint16_t slotNo) const;
        /// True if slot `slotNo` holds a record (offset != 0). Same range
        /// rule as slot().
        bool isLive(std::uint16_t slotNo) const;

        /// Stores `recordBytes` and returns its new slot number, or an empty
        /// optional (NO_SPACE) if free space < L + 4. `recordBytes` is one
        /// record already encoded by encodeRecord (record_codec.h), so
        /// L = recordBytes.size(); the page copies the bytes and never looks
        /// at the fields. Follows the §M1.4 pseudocode: free_end -= L, copy
        /// the record to free_end, append slot (free_end, L). Always appends a
        /// new slot; dead slots are never reused. No space is a normal result,
        /// not an error.
        std::optional<std::uint16_t> insert(std::span<const std::byte> recordBytes);

        /// The bytes of the record in slot `slotNo`, as a view into the page.
        /// Valid only while the page bytes stay alive and unchanged: once a
        /// buffer-pool frame is unpinned it may hold another page, so decode
        /// (copy) the record before unpinning. Throws db::Error(Name) if the
        /// slot does not exist or is dead.
        std::span<const std::byte> get(std::uint16_t slotNo) const;

        /// Marks slot `slotNo` dead by setting it to (0, 0). free_end and the
        /// free space do not change (no compaction). Throws db::Error(Name)
        /// if the slot does not exist or is already dead.
        void remove(std::uint16_t slotNo);

    private:
        std::span<std::byte> bytes_;
    };
} // namespace db::storage
