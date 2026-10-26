#include "db/storage/heap_file.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "db/common/error.h"
#include "db/storage/page_guard.h"
#include "db/storage/record_codec.h"
#include "db/storage/slotted_page.h"

namespace db::storage
{
    namespace
    {
        // Directory pages sit at 0, 682, 1364, ... (see heap_file.h).
        bool isDirectoryPage(PageId pageId)
        {
            return pageId % kDirStride == 0;
        }

        // The directory page that lists data page `dataPageId`.
        PageId directoryPageOf(PageId dataPageId)
        {
            return dataPageId / kDirStride * kDirStride;
        }

        // The entry number of `dataPageId` on its directory page: data page
        // 1 is entry 0 of page 0, data page 683 is entry 0 of page 682.
        std::uint16_t entryIndexOf(PageId dataPageId)
        {
            return static_cast<std::uint16_t>(dataPageId - directoryPageOf(dataPageId) - 1);
        }

        // "(42, bob, 7)": the row as text, for the "record too long" message.
        std::string rowText(const Tuple& tuple)
        {
            std::string text = "(";
            for (std::size_t i = 0; i < tuple.size(); ++i)
            {
                if (i > 0)
                    text += ", ";
                text += tuple[i].toText();
            }
            return text + ")";
        }
    } // namespace

    void HeapFile::create(BufferPool& pool, FileId fileId)
    {
        if (pool.numPages(fileId) != 0)
            throw std::logic_error("HeapFile::create: file " + std::to_string(fileId) + " already has "
                + std::to_string(pool.numPages(fileId)) + " pages");

        // NOTE: PageGuard::~PageGuard() noexcept
        // {
        //     release();
        // }
        // It just calls release(). same applies everywhere eit is used in this file
        // hence everytime it exist block or function level scope it releases pin automatically
        PageGuard guard = PageGuard::newPage(pool, fileId);
        // The file was empty, so the new page is page 0 (§M1.5: "Page 0 of
        // every table file is the first directory page").
        // write an empty directory header on it: page id 0, no next page, 0 entries.
        DirectoryPage(guard.data()).init(guard.pageId());
        guard.markDirty();
    }

    HeapFile::HeapFile(BufferPool& pool, FileId fileId, Schema schema, std::string tableName)
        : pool_(&pool), fileId_(fileId), schema_(std::move(schema)), tableName_(std::move(tableName))
    {
    }

    Rid HeapFile::insert(const Tuple& tuple)
    {
        // 1. Encode. This also checks the tuple against the schema, before
        //    any page is touched.
        const std::vector<std::byte> record = encodeRecord(tuple, schema_);
        // A record needs its own bytes plus a 4-byte slot. For our example, 13 + 4 = 17
        const std::size_t needed = record.size() + kSlotSize;

        // 2. An empty page has 4088 bytes free, so a record needing more can
        //    never be stored if needed was say 4085 size then throw error
        if (needed > kPageSize - kPageHeaderSize)
            throw Error(ErrorCategory::Schema,
                        "Table '" + tableName_ + "': record too long (" + std::to_string(record.size())
                        + " bytes, at most " + std::to_string(kPageSize - kPageHeaderSize - kSlotSize)
                        + " fit on a page): row " + rowText(tuple));

        // 3. First fit: walk the directory pages in order, holding one at a time.
        // and pins the page it uses so pin 1
        PageGuard dirGuard = PageGuard::fetch(*pool_, fileId_, 0);
        while (true)
        {
            DirectoryPage dir(dirGuard.data());
            for (std::uint16_t i = 0; i < dir.entryCount(); ++i)
            {
                const DirectoryEntry entry = dir.entry(i);
                if (entry.freeBytes < needed)
                    continue;

                // 4. Found: second pin, on the data page.
                PageGuard dataGuard = PageGuard::fetch(*pool_, fileId_, entry.dataPageId);

                SlottedPage page(dataGuard.data());
                const auto slot = page.insert(record);

                if (!slot)
                    throw std::runtime_error("HeapFile: table '" + tableName_ + "': directory says page "
                        + std::to_string(entry.dataPageId) + " has " + std::to_string(entry.freeBytes)
                        + " bytes free, but the page has " + std::to_string(page.freeSpace()));
                dir.setFreeBytes(i, page.freeSpace());
                dataGuard.markDirty();
                dirGuard.markDirty();
                return Rid{entry.dataPageId, *slot}; // both guards unpin here
            }

            const PageId next = dir.nextDirPage();
            if (next == kInvalidPageId)
                break; // dirGuard holds the last directory page
            // Unpin this directory page before pinning the next one.
            dirGuard.release();
            dirGuard = PageGuard::fetch(*pool_, fileId_, next); // pin 1 on next page
        }

        // 5. No page fits and the last directory page is full: add a new
        //    directory page first and link it, then let go of the old one.
        if (DirectoryPage(dirGuard.data()).isFull())
        {
            PageGuard newDirGuard = PageGuard::newPage(*pool_, fileId_);
            // write an emtpy directory header on it: page id 0, no next page, 0 entries.
            DirectoryPage(newDirGuard.data()).init(newDirGuard.pageId());
            newDirGuard.markDirty();
            // on the old directory page, set "next directory page" to the new page's number.
            DirectoryPage(dirGuard.data()).setNextDirPage(newDirGuard.pageId());
            // mark the old page dirty.
            dirGuard.markDirty();
            // Move assignment unpins the old directory page and keeps the new one.
            dirGuard = std::move(newDirGuard);
        }

        // 6. Add a new data page at the end of the file and list it on the
        //    last directory page.
        PageGuard dataGuard = PageGuard::newPage(*pool_, fileId_);
        // get, remove and numDataPages find pages by arithmetic (heap_file.h),
        // so the new data page must land exactly where the arithmetic says:
        // the next entry of the directory page we hold. This fails only if the
        // file already holds a page the directory does not list (e.g. it was
        // added but the directory change was never flushed). Checked before
        // the entry is added, so the directory is left as it was.
        DirectoryPage dir(dirGuard.data());
        const PageId newId = dataGuard.pageId();
        if (directoryPageOf(newId) != dirGuard.pageId() || entryIndexOf(newId) != dir.entryCount())
            throw std::runtime_error("HeapFile: table '" + tableName_ + "': new data page "
                + std::to_string(newId) + " should be entry " + std::to_string(dir.entryCount())
                + " of directory page " + std::to_string(dirGuard.pageId())
                + "; the file holds pages its directory does not list");
        SlottedPage page(dataGuard.data());
        page.init(dataGuard.pageId());
        // Cannot fail: the record needs at most 4088 bytes (step 2) and the page is empty.
        const auto slot = page.insert(record);
        if (!slot)
            throw std::logic_error("HeapFile::insert: record of " + std::to_string(record.size())
                + " bytes did not fit an empty page");
        dir.addEntry(newId, page.freeSpace());
        dataGuard.markDirty();
        dirGuard.markDirty();
        return Rid{dataGuard.pageId(), *slot};
    }

    void HeapFile::requireDataPage(Rid rid) const
    {
        if (rid.pageId >= pool_->numPages(fileId_) || isDirectoryPage(rid.pageId))
            throw Error(ErrorCategory::Name, "Table '" + tableName_ + "': no record at rid " + rid.toString()
                        + " (page " + std::to_string(rid.pageId) + " is not a data page of the table).");
    }

    namespace
    {
        // Throws Error(Name) naming the table and the rid unless the slot
        // exists and is live (§M1.8 "Reading a dead slot -> error naming the rid").
        void requireLiveSlot(const SlottedPage& page, Rid rid, const std::string& tableName)
        {
            if (rid.slot >= page.slotCount())
                throw Error(ErrorCategory::Name, "Table '" + tableName + "': no record at rid " + rid.toString()
                            + " (page has " + std::to_string(page.slotCount()) + " slots).");
            if (!page.isLive(rid.slot))
                throw Error(ErrorCategory::Name,
                            "Table '" + tableName + "': no record at rid " + rid.toString() + " (it was deleted).");
        }
    } // namespace

    Tuple HeapFile::get(Rid rid) const
    {
        requireDataPage(rid);
        // One page access: fetch, read the slot, decode, unpin (§M1.5).
        PageGuard guard = PageGuard::fetch(*pool_, fileId_, rid.pageId);
        const SlottedPage page(guard.data());
        requireLiveSlot(page, rid, tableName_);
        // decodeRecord copies the bytes out, so the tuple stays valid after the unpin.
        return decodeRecord(page.get(rid.slot), schema_);
    }

    void HeapFile::remove(Rid rid)
    {
        requireDataPage(rid);
        PageGuard dataGuard = PageGuard::fetch(*pool_, fileId_, rid.pageId);
        SlottedPage page(dataGuard.data());
        requireLiveSlot(page, rid, tableName_);
        page.remove(rid.slot);
        dataGuard.markDirty();
        const std::uint16_t freeBytes = page.freeSpace();
        dataGuard.release();

        // §M1.5 "update directory free_bytes". Without compaction (D3) the
        // value is the same, but we follow the pseudocode, so this stays
        // right if compaction is ever added.
        // pin the directory page that lists this data page (directoryPageOf). Pins: 1
        PageGuard dirGuard = PageGuard::fetch(*pool_, fileId_, directoryPageOf(rid.pageId));
        DirectoryPage dir(dirGuard.data());
        const std::uint16_t index = entryIndexOf(rid.pageId);
        if (index >= dir.entryCount() || dir.entry(index).dataPageId != rid.pageId)
            throw std::runtime_error("HeapFile: table '" + tableName_ + "': directory page "
                + std::to_string(dirGuard.pageId()) + " does not list data page " + std::to_string(rid.pageId)
                + " at entry " + std::to_string(index));
        dir.setFreeBytes(index, freeBytes);
        dirGuard.markDirty();
    }

    HeapScan HeapFile::scan() const
    {
        return HeapScan(*pool_, fileId_, schema_);
    }

    std::uint32_t HeapFile::numDataPages() const
    {
        // Directory pages are the multiples of 682 below the page count:
        // ceil(pages / 682) of them. Everything else is a data page.
        const PageId pages = pool_->numPages(fileId_);
        const PageId directoryPages = (pages + kDirStride - 1) / kDirStride;
        //  data pages = all pages − directory pages.
        return pages - directoryPages;
    }
} // namespace db::storage
