#include "db/storage/catalog.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog_format.h"
#include "db/common/error.h"
#include "db/storage/heap_file.h"
#include "db/storage/page_guard.h"
#include "db/storage/slotted_page.h"

namespace db::storage
{
    namespace format = catalog_format;

    namespace
    {
        bool isNameCharacter(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        // The name in lower case. Only ASCII letters change, and a valid
        // table name holds nothing else, so no locale is involved.
        std::string lowerCase(const std::string& name)
        {
            std::string result = name;
            for (char& c : result)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return result;
        }

        // The same columns without qualifiers (`R.a` -> `a`). The Schema
        // constructor rejects two columns that now share a name.
        Schema bareSchema(const Schema& schema)
        {
            std::vector<Column> columns = schema.columns();
            for (Column& column : columns)
                column.qualifier.clear();
            return Schema(std::move(columns));
        }

        // "catalog.db page 2 slot 5: " + the format's message, so a corrupt
        // file says where the bad record is.
        [[noreturn]] void rethrowAt(const std::exception& error, PageId page, std::uint16_t slot)
        {
            throw std::runtime_error("catalog.db page " + std::to_string(page) + " slot " + std::to_string(slot)
                + ": " + error.what());
        }
    } // namespace

    Catalog::Catalog(std::filesystem::path dbDir, FileRegistry& files, BufferPool& pool, FileId catalogFile)
        : dbDir_(std::move(dbDir)), files_(files), pool_(&pool), catalogFile_(catalogFile)
    {
    }

    void Catalog::usePool(BufferPool& pool)
    {
        pool_ = &pool;
    }

    bool Catalog::changed() const
    {
        return changed_;
    }

    // --- load -------------------------------------------------------------------

    void Catalog::load()
    {
        // Build everything in local variables first and only replace the map
        // at the end, so a corrupt file leaves the catalog as it was.
        std::map<std::string, TableInfo> tables;
        // Statistics chunks per table, in the order they were read.
        std::map<std::string, std::vector<format::StatsChunk>> chunks;

        const PageId pageCount = pool_->numPages(catalogFile_);
        for (PageId p = 0; p < pageCount; ++p)
        {
            // One page pinned at a time: the guard is released at the end of
            // each loop turn, after every record on the page has been copied
            // out (decoding copies the bytes).
            PageGuard guard = PageGuard::fetch(*pool_, catalogFile_, p);
            const SlottedPage page(guard.data());
            for (std::uint16_t s = 0; s < page.slotCount(); ++s)
            {
                if (!page.isLive(s))
                    continue;
                const std::span<const std::byte> record = page.get(s);
                try
                {
                    const std::uint8_t kind = format::recordKind(record);
                    if (kind == format::kTableRecord)
                    {
                        TableInfo info = format::decodeTableRecord(record);
                        const std::string name = info.name;
                        if (!tables.emplace(name, std::move(info)).second)
                            throw std::runtime_error("Corrupt catalog: table '" + name + "' is listed twice");
                    }
                    else if (kind == format::kStatsChunk)
                    {
                        format::StatsChunk chunk = format::decodeStatsChunk(record);
                        chunks[chunk.tableName].push_back(std::move(chunk));
                    }
                    else
                        throw std::runtime_error("Corrupt catalog: unknown record kind " + std::to_string(kind));
                }
                catch (const std::runtime_error& error)
                {
                    rethrowAt(error, p, s);
                }
            }
        }

        // Statistics last: decoding a blob needs the table's schema, and the
        // table record may be on any page.
        for (const auto& [name, tableChunks] : chunks)
        {
            const auto it = tables.find(name);
            if (it == tables.end())
                throw std::runtime_error("Corrupt catalog: statistics for unknown table '" + name + "'");
            const std::vector<std::byte> blob = format::joinChunks(tableChunks);
            it->second.stats = format::decodeStats(blob, it->second.schema);
        }

        tables_ = std::move(tables);
        changed_ = false;
    }

    // --- save -------------------------------------------------------------------

    void Catalog::save()
    {
        if (!changed_)
            return;

        // 1. Every record, in the fixed order: table records by name (the
        //    map's order), then the statistics chunks of each analyzed table,
        //    again by name, each table's chunks by chunk number.
        std::vector<std::vector<std::byte>> records;
        for (const auto& [name, info] : tables_)
            records.push_back(format::encodeTableRecord(info));
        for (const auto& [name, info] : tables_)
        {
            if (!info.stats)
                continue;
            const std::vector<std::byte> blob = format::encodeStats(*info.stats, info.schema);
            for (std::vector<std::byte>& chunk : format::cutIntoChunks(name, blob))
                records.push_back(std::move(chunk));
        }

        // 2. Fill pages from page 0. Existing pages are reused (fetch), new
        //    ones added at the end (newPage). Each page is zeroed before it is
        //    formatted, so old record bytes never survive: the file's bytes
        //    depend only on the catalog's content (byte-identical files).
        const PageId oldPageCount = pool_->numPages(catalogFile_);
        PageId pageNo = 0;
        // Opens page `pageNo` as an empty slotted page, holding its pin.
        auto startPage = [&]() {
            PageGuard guard = pageNo < oldPageCount ? PageGuard::fetch(*pool_, catalogFile_, pageNo)
                                                    : PageGuard::newPage(*pool_, catalogFile_);
            std::fill(guard.data().begin(), guard.data().end(), std::byte{0});
            SlottedPage(guard.data()).init(guard.pageId());
            guard.markDirty();
            return guard;
        };

        PageGuard guard = startPage();
        for (const std::vector<std::byte>& record : records)
        {
            if (SlottedPage(guard.data()).insert(record))
                continue;
            // The page is full: release it, then continue on the next page.
            guard.release();
            ++pageNo;
            guard = startPage();
            // Every record is at most 4084 bytes, so it always fits an empty page.
            if (!SlottedPage(guard.data()).insert(record))
                throw std::logic_error("Catalog::save: a record of " + std::to_string(record.size())
                    + " bytes does not fit on an empty page");
        }
        guard.release();

        // 3. Pages left over from a bigger catalog become empty pages, so
        //    load() never sees records that are no longer there.
        for (++pageNo; pageNo < oldPageCount; ++pageNo)
            startPage(); // the returned guard is released right away

        changed_ = false;
    }

    // --- Tables -------------------------------------------------------------------

    void Catalog::createTable(const std::string& name, const Schema& schema)
    {
        if (name.empty() || !std::all_of(name.begin(), name.end(), isNameCharacter))
            throw Error(ErrorCategory::Schema,
                        "Invalid table name '" + name + "' (use only letters, digits and _).");
        if (lowerCase(name) == "catalog")
            throw Error(ErrorCategory::Schema,
                        "Invalid table name '" + name + "' (reserved: the catalog is stored in catalog.db).");
        if (tables_.contains(name))
            throw Error(ErrorCategory::Schema, "Table '" + name + "' already exists.");
        // No check for names that differ only in case: identifiers are
        // case-sensitive and `Emp` and `emp` are different tables (sql SPEC,
        // DECISIONS 2026-10-30). On a case-insensitive file system (Windows)
        // the second one fails safely below: FileRegistry::create never
        // overwrites an existing file, so it throws Error(Name) "already
        // exists" and nothing changes.
        if (schema.size() == 0)
            throw Error(ErrorCategory::Schema, "Table '" + name + "' must have at least one column.");

        TableInfo info;
        info.name = name;
        info.schema = bareSchema(schema);
        info.dataFile = name + ".db";
        info.firstPage = 0;

        // The record must fit on one catalog page (records never span pages).
        const std::size_t recordSize = format::encodeTableRecord(info).size();
        if (recordSize > format::kMaxCatalogRecord)
            throw Error(ErrorCategory::Schema, "Table '" + name + "': its definition needs "
                + std::to_string(recordSize) + " bytes in the catalog, at most "
                + std::to_string(format::kMaxCatalogRecord) + " fit on a page.");

        // Every check passed: now create the file and format page 0.
        const FileId fileId = files_.create(dbDir_ / info.dataFile);
        HeapFile::create(*pool_, fileId);

        tables_.emplace(name, std::move(info));
        changed_ = true;
    }

    std::optional<TableInfo> Catalog::getTable(const std::string& name) const
    {
        const auto it = tables_.find(name);
        if (it == tables_.end())
            return std::nullopt;
        TableInfo copy = it->second;
        copy.schema = requalify(copy.schema, name);
        return copy;
    }

    bool Catalog::hasTable(const std::string& name) const
    {
        return tables_.contains(name);
    }

    std::vector<std::string> Catalog::listTables() const
    {
        std::vector<std::string> names;
        for (const auto& [name, info] : tables_)
            names.push_back(name);
        return names;
    }

    TableInfo& Catalog::entry(const std::string& name)
    {
        const auto it = tables_.find(name);
        if (it == tables_.end())
            throw Error(ErrorCategory::Name, "Unknown table '" + name + "'.");
        return it->second;
    }

    void Catalog::updateCounts(const std::string& name, std::uint32_t numPages, std::uint64_t numTuples)
    {
        TableInfo& info = entry(name);
        info.numPages = numPages;
        info.numTuples = numTuples;
        changed_ = true;
    }

    void Catalog::setStats(const std::string& name, const TableStats& stats)
    {
        TableInfo& info = entry(name);
        // Encoding checks the column count and every value's type, and that
        // the chunks can be numbered; doing it now means save() can never
        // fail on these statistics later.
        format::cutIntoChunks(name, format::encodeStats(stats, info.schema));
        info.stats = stats;
        changed_ = true;
    }
} // namespace db::storage
