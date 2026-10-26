#include "db/storage/directory_page.h"

#include <stdexcept>
#include <string>

#include "db/storage/bytes.h"

namespace db::storage
{
    namespace
    {
        // Where each header field starts on the page (§M1.5).
        constexpr std::size_t kPageIdOffset = 0;
        constexpr std::size_t kNextDirPageOffset = 4;
        constexpr std::size_t kEntryCountOffset = 8;

        // Where entry `index` starts: 10 + 6 * index (entry 0 at 10, entry 1
        // at 16, ...). Inside an entry, the data page id comes first (4
        // bytes), then free_bytes, so free_bytes is at position + 4.
        std::size_t entryPosition(std::uint16_t index)
        {
            return kDirHeaderSize + kDirEntrySize * index;
        }
    } // namespace

    DirectoryPage::DirectoryPage(std::span<std::byte> bytes) : bytes_(bytes)
    {
        if (bytes_.size() != kPageSize)
            throw std::logic_error("DirectoryPage: view of " + std::to_string(bytes_.size()) + " bytes, expected "
                + std::to_string(kPageSize));
    }

    void DirectoryPage::init(PageId pageId)
    {
        writeU32(bytes_, kPageIdOffset, pageId);
        writeU32(bytes_, kNextDirPageOffset, kInvalidPageId);
        writeU16(bytes_, kEntryCountOffset, 0);
    }

    PageId DirectoryPage::pageId() const
    {
        return readU32(bytes_, kPageIdOffset);
    }

    PageId DirectoryPage::nextDirPage() const
    {
        return readU32(bytes_, kNextDirPageOffset);
    }

    void DirectoryPage::setNextDirPage(PageId next)
    {
        writeU32(bytes_, kNextDirPageOffset, next);
    }

    std::uint16_t DirectoryPage::entryCount() const
    {
        const std::uint16_t count = readU16(bytes_, kEntryCountOffset);
        // At most 681 entries fit. A bigger count would make us read past
        // the end of the page, so the page must be corrupt. Every function
        // that touches entries calls this first, so they are all protected.
        if (count > kDirEntriesPerPage)
            throw std::runtime_error("Corrupt directory page " + std::to_string(pageId()) + ": entry_count "
                + std::to_string(count) + " exceeds " + std::to_string(kDirEntriesPerPage));
        return count;
    }

    bool DirectoryPage::isFull() const
    {
        return entryCount() == kDirEntriesPerPage;
    }

    DirectoryEntry DirectoryPage::entry(std::uint16_t index) const
    {
        if (index >= entryCount())
            throw std::logic_error("DirectoryPage::entry: entry " + std::to_string(index) + " of directory page "
                + std::to_string(pageId()) + " does not exist (" + std::to_string(entryCount()) + " entries)");
        const std::size_t position = entryPosition(index);
        return DirectoryEntry{readU32(bytes_, position), readU16(bytes_, position + 4)};
    }

    std::uint16_t DirectoryPage::addEntry(PageId dataPageId, std::uint16_t freeBytes)
    {
        // The new entry goes right after the last one, so its number is the
        // current count.
        const std::uint16_t index = entryCount();
        if (index == kDirEntriesPerPage)
            throw std::logic_error("DirectoryPage::addEntry: directory page " + std::to_string(pageId())
                + " is full (" + std::to_string(kDirEntriesPerPage) + " entries)");
        // Always added at the end, never in the middle: the entry order is
        // the order a scan reads the table.
        const std::size_t position = entryPosition(index);
        writeU32(bytes_, position, dataPageId);
        writeU16(bytes_, position + 4, freeBytes);
        writeU16(bytes_, kEntryCountOffset, static_cast<std::uint16_t>(index + 1));
        return index;
    }

    void DirectoryPage::setFreeBytes(std::uint16_t index, std::uint16_t freeBytes)
    {
        if (index >= entryCount())
            throw std::logic_error("DirectoryPage::setFreeBytes: entry " + std::to_string(index)
                + " of directory page " + std::to_string(pageId()) + " does not exist ("
                + std::to_string(entryCount()) + " entries)");
        // Only the 2 free_bytes bytes change; the data page id stays.
        writeU16(bytes_, entryPosition(index) + 4, freeBytes);
    }

} // namespace db::storage
