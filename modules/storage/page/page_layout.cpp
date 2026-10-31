#include "db/storage/page_layout.h"

#include <string>
#include <utility>

#include "db/common/error.h"
#include "db/storage/heap_file.h"
#include "db/storage/page_guard.h"
#include "db/storage/record_codec.h"

namespace db::storage
{
    namespace
    {
        // Copies the header and every slot of a data page into `layout`.
        // Each live record is decoded now, while the page is still pinned;
        // decodeRecord copies the bytes, so the tuple owns its strings.
        void describeDataPage(const SlottedPage& page, const Schema& schema, PageLayout& layout)
        {
            layout.kind = PageKind::Data;
            layout.pageId = page.pageId();
            layout.slotCount = page.slotCount();
            layout.freeEnd = page.freeEnd();
            layout.freeSpace = page.freeSpace();
            for (std::uint16_t i = 0; i < layout.slotCount; ++i)
            {
                const SlotEntry entry = page.slot(i);
                SlotLayout slot;
                slot.slot = i;
                slot.entryOffset = kDataSlotsOffset + kSlotSize * i;
                slot.offset = entry.offset;
                slot.length = entry.length;
                slot.live = page.isLive(i);
                if (slot.live)
                    slot.values = decodeRecord(page.get(i), schema);
                layout.slots.push_back(std::move(slot));
            }
        }

        // Copies the header and every entry of a directory page into `layout`.
        void describeDirectoryPage(const DirectoryPage& page, PageLayout& layout)
        {
            layout.kind = PageKind::Directory;
            layout.pageId = page.pageId();
            layout.nextDirPage = page.nextDirPage();
            layout.entryCount = page.entryCount();
            for (std::uint16_t i = 0; i < layout.entryCount; ++i)
            {
                const DirectoryEntry entry = page.entry(i);
                layout.entries.push_back(
                    DirEntryLayout{i, kDirEntriesOffset + kDirEntrySize * i, entry.dataPageId, entry.freeBytes});
            }
        }
    } // namespace

    PageLayout describePage(StorageEngine& engine, const std::string& table, PageId pageId)
    {
        // openHeapFile throws Error(Name) "Unknown table 'X'." for us and
        // gives the file id and the table's schema (to decode the rows).
        const HeapFile heap = engine.openHeapFile(table);
        BufferPool& pool = engine.pool();

        PageLayout layout;
        layout.table = table;
        layout.filePages = pool.numPages(heap.fileId());
        // Checked here, before the fetch, so the user gets a message naming
        // the table instead of the pool's internal logic_error.
        if (pageId >= layout.filePages)
            throw Error(ErrorCategory::Name,
                        "Table '" + table + "' has " + std::to_string(layout.filePages)
                        + " pages; there is no page " + std::to_string(pageId) + ".");

        // One pin for the whole description; the guard unpins (clean) when
        // the function returns or throws.
        // Never marked dirty: the helpers take const views and only read.
        PageGuard guard = PageGuard::fetch(pool, heap.fileId(), pageId);
        if (pageId % kDirStride == 0) // the 682 rule of heap_file.h
            describeDirectoryPage(DirectoryPage(guard.data()), layout);
        else
            describeDataPage(SlottedPage(guard.data()), heap.schema(), layout);
        return layout;
    }
} // namespace db::storage
