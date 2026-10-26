#include "db/storage/slotted_page.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "db/common/error.h"
#include "db/storage/bytes.h"

namespace db::storage
{
    namespace
    {
        // Header field positions (§M1.4).
        constexpr std::size_t kPageIdOffset = 0;
        // says where in the header the slotCount number is stored: its 2 bytes start at byte 4.
        constexpr std::size_t kSlotCountOffset = 4;
        // says where in the header the free_end number is stored: its 2 bytes start at byte 6.
        constexpr std::size_t kFreeEndOffset = 6;

        // Byte position of slot `slotNo` in the directory: slot 0 @8, slot 1 @12, ...
        std::size_t slotPosition(std::uint16_t slotNo)
        {
            return kPageHeaderSize + kSlotSize * slotNo;
        }

        // A rid as error messages show it: "(page_id, slot)".
        std::string ridText(PageId pageId, std::uint16_t slotNo)
        {
            return "(" + std::to_string(pageId) + ", " + std::to_string(slotNo) + ")";
        }

        // Throws db::Error(Name) naming the rid unless slot `slotNo` exists
        // and is live (§M1.8 "Reading a dead slot: error naming the rid").
        // Existence is checked first: isLive on a missing slot would throw a
        // logic_error instead of the user-facing error.
        void requireLive(const SlottedPage& page, std::uint16_t slotNo)
        {
            if (slotNo >= page.slotCount())
                throw Error(ErrorCategory::Name, "Record " + ridText(page.pageId(), slotNo)
                            + " does not exist (page has "
                            + std::to_string(page.slotCount()) + " slots).");
            if (!page.isLive(slotNo))
                throw Error(ErrorCategory::Name, "Record " + ridText(page.pageId(), slotNo) + " is deleted.");
        }
    } // namespace

    SlottedPage::SlottedPage(std::span<std::byte> bytes) : bytes_(bytes)
    {
        if (bytes_.size() != kPageSize)
            throw std::logic_error("SlottedPage: view of " + std::to_string(bytes_.size()) + " bytes, expected "
                + std::to_string(kPageSize));
    }

    // initialize the page
    void SlottedPage::init(PageId pageId)
    {
        writeU32(bytes_, kPageIdOffset, pageId);
        writeU16(bytes_, kSlotCountOffset, 0);
        // 4096 fits a uint16 (max 65535): an empty page's data starts at the end.
        writeU16(bytes_, kFreeEndOffset, static_cast<std::uint16_t>(kPageSize));
    }

    // The getters read the bytes every time: nothing is cached in the view.
    PageId SlottedPage::pageId() const
    {
        return readU32(bytes_, kPageIdOffset);
    }

    std::uint16_t SlottedPage::slotCount() const
    {
        return readU16(bytes_, kSlotCountOffset);
    }

    std::uint16_t SlottedPage::freeEnd() const
    {
        return readU16(bytes_, kFreeEndOffset);
    }

    // Offset 0 means dead: a live record always starts after the header and
    // the slot directory, so never at byte 0.
    bool SlottedPage::isLive(std::uint16_t slotNo) const
    {
        return slot(slotNo).offset != 0;
    }

    std::uint16_t SlottedPage::freeSpace() const
    {
        // free_end - (8 + 4 * slot_count) (§M1.4). Checked before subtracting:
        // on a corrupt page the unsigned result would wrap to a huge number
        // and the page would look almost empty.
        const std::size_t directoryEnd = kPageHeaderSize + kSlotSize * slotCount(); // 8 + 4 × slot_count
        // free_end: where the record data begins (the top of the free gap)
        const std::size_t end = freeEnd();
        if (end < directoryEnd)
            throw std::runtime_error("Corrupt page " + std::to_string(pageId()) + ": free_end "
                + std::to_string(end) + " is inside the slot directory (ends at "
                + std::to_string(directoryEnd) + ").");
        // At most 4096 - 8 = 4088, so it fits a uint16.
        return static_cast<std::uint16_t>(end - directoryEnd);
    }

    SlotEntry SlottedPage::slot(std::uint16_t slotNo) const
    {
        if (slotNo >= slotCount())
            throw std::logic_error("SlottedPage::slot: slot " + std::to_string(slotNo) + " of page "
                + std::to_string(pageId()) + " does not exist ("
                + std::to_string(slotCount()) + " slots)");
        const std::size_t position = slotPosition(slotNo);
        return SlotEntry{readU16(bytes_, position), readU16(bytes_, position + 2)};
    }


    std::optional<std::uint16_t> SlottedPage::insert(std::span<const std::byte> recordBytes)
    {
        // `recordBytes` is one tuple already encoded by encodeRecord
        // (record_codec.h, §M1.3); the heap file (chunk 08) encodes and then
        // calls insert. The page only copies bytes, so its size in bytes is L.
        // Kept as size_t until the space check: a record longer than 65535
        // bytes would be cut short by an early uint16 cast.
        const std::size_t length = recordBytes.size();

        // L bytes for the record + 4 for its new slot entry (§M1.4). After
        // this check length <= 4084, so the uint16 casts below are safe.
        if (length + kSlotSize > freeSpace())
            return std::nullopt;

        // Always a new slot at the end: dead slots are never reused.
        const std::uint16_t slotNo = slotCount();
        const auto newFreeEnd = static_cast<std::uint16_t>(freeEnd() - length);

        // The record goes at the top of the free space, its slot entry at the
        // bottom; the check above guarantees they do not overlap.
        // used std::copy here which does transfer to *dest = *src
        std::copy(recordBytes.begin(), recordBytes.end(), bytes_.begin() + newFreeEnd);
        const std::size_t position = slotPosition(slotNo);
        writeU16(bytes_, position, newFreeEnd);
        writeU16(bytes_, position + 2, static_cast<std::uint16_t>(length));

        writeU16(bytes_, kFreeEndOffset, newFreeEnd);
        writeU16(bytes_, kSlotCountOffset, static_cast<std::uint16_t>(slotNo + 1));
        return slotNo;
    }

    std::span<const std::byte> SlottedPage::get(std::uint16_t slotNo) const
    {
        requireLive(*this, slotNo);
        const SlotEntry entry = slot(slotNo);
        // A live record lies in the record data area [free_end, 4096); a slot
        // pointing elsewhere would make subspan read outside the page.
        if (entry.offset < freeEnd() || entry.offset + entry.length > kPageSize)
            throw std::runtime_error("Corrupt page " + std::to_string(pageId()) + ": slot "
                + std::to_string(slotNo) + " (" + std::to_string(entry.offset) + ", "
                + std::to_string(entry.length) + ") is outside the record data.");

        // A view into the page, not a copy: valid only while the frame holds
        // this page, so callers decode it before unpinning.
        return bytes_.subspan(entry.offset, entry.length);
    }

    void SlottedPage::remove(std::uint16_t slotNo)
    {
        requireLive(*this, slotNo);
        // Only the slot changes; free_end and slot_count stay (no compaction,
        // D3), and the slot number is never reused, so other rids stay valid.
        const std::size_t position = slotPosition(slotNo);
        writeU16(bytes_, position, 0);
        writeU16(bytes_, position + 2, 0);
    }
} // namespace db::storage
