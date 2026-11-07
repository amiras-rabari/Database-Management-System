#include "sort/run_file.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "db/common/error.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/record_codec.h"
#include "db/storage/slotted_page.h"

namespace db::exec
{
    // ---------------------------------------------------------------- writer

    RunWriter::RunWriter(storage::StorageEngine& engine, const Schema& schema)
        : engine_(engine), schema_(schema), file_(engine.createTempFile())
    {
    }

    void RunWriter::append(const Tuple& tuple)
    {
        const std::vector<std::byte> bytes = storage::encodeRecord(tuple, schema_);

        // An empty page has 4096 - 8 = 4088 bytes free, and a record needs
        // its bytes plus a 4-byte slot, so at most 4084 bytes fit. Checked
        // before anything changes, so the run stays as it was.
        if (bytes.size() + storage::kSlotSize > storage::kPageSize - storage::kPageHeaderSize)
            throw Error(ErrorCategory::Schema, "A tuple of " + std::to_string(bytes.size()) +
                                                   " bytes is too long for a temporary page (at most 4084 bytes).");

        // Try the current page first. No space: write it and start a new one.
        if (page_.valid())
        {
            if (storage::SlottedPage(page_.data()).insert(bytes))
                return;
            writeCurrentPage();
        }
        // newPage adds page 0, 1, 2, ... to the file. It counts no write and
        // is already dirty in the pool.
        page_ = storage::PageGuard::newPage(engine_.pool(), file_.fileId());
        storage::SlottedPage page(page_.data());
        page.init(page_.pageId());
        page.insert(bytes); // always fits on an empty page (checked above)
    }

    storage::TempFile RunWriter::finish()
    {
        if (page_.valid())
            writeCurrentPage();
        // Every page is on disk and unpinned. Drop them from the pool, so the
        // next pass reads each page from disk (a counted read, as the
        // formulas assume) instead of finding it cached (Q3 (a)).
        engine_.pool().discardFile(file_.fileId());
        return std::move(file_);
    }

    void RunWriter::writeCurrentPage()
    {
        const storage::PageId pageId = page_.pageId();
        // Unpin first, then write. Nothing runs in between, so the page is
        // still in the pool, and flushPage writes it now: exactly 1 write.
        // Without this, a dirty page might never be written before the run
        // is deleted, and the measured writes would be fewer than the formula.
        page_.release();
        engine_.pool().flushPage(file_.fileId(), pageId);
    }

    // ---------------------------------------------------------------- reader

    RunReader::RunReader(storage::StorageEngine& engine, const storage::TempFile& run, const Schema& schema)
        : engine_(engine), run_(run), schema_(schema)
    {
    }

    bool RunReader::next(Tuple& out)
    {
        // Each loop either returns a tuple from the pinned page, or moves to
        // the next page, or stops at the end of the run.
        while (true)
        {
            if (page_.valid())
            {
                const storage::SlottedPage page(page_.data());
                if (nextSlot_ < page.slotCount())
                {
                    // Decode copies the values out, so `out` stays valid
                    // after the page is unpinned.
                    out = storage::decodeRecord(page.get(nextSlot_), schema_);
                    ++nextSlot_;
                    return true;
                }
                page_.release(); // this page is used up
            }
            if (nextPage_ == run_.numPages())
                return false; // the end: no page pinned
            page_ = storage::PageGuard::fetch(engine_.pool(), run_.fileId(), nextPage_);
            ++nextPage_;
            nextSlot_ = 0;
        }
    }
} // namespace db::exec
