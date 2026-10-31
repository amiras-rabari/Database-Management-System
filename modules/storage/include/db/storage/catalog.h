#pragma once

/// @file
/// The catalog (§M1.7, chunk 12): which tables exist, their columns, files,
/// page and tuple counts, and their statistics.
///
/// It lives in the pages of `<DIR>/catalog.db` (format: catalog/catalog_format.h,
/// DESIGN_M1.md) and is read through the buffer pool. To keep catalog pages
/// out of every command's counters (D4, DECISIONS 2026-10-25):
/// - load() reads every catalog page once, when the engine opens, into an
///   in-memory map (table metadata only, never tuples);
/// - every other call works on that map and does no I/O, except
///   createTable, which creates and formats the new table's file;
/// - save() writes the map back, only if something changed, when the
///   engine runs flushAll() or close().
///
/// Normally reached through StorageEngine::catalog(); the engine builds it.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "db/common/schema.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/page.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::storage
{
    class StorageEngine;

    /// Not copyable or movable: it is part of one engine and points at its
    /// registry and pool.
    ///
    /// Errors:
    /// - createTable: bad name, the name is taken (exact match), a bad
    ///   schema → db::Error(Schema);
    /// - updateCounts / setStats on a table that does not exist →
    ///   db::Error(Name) "Unknown table 'X'.";
    /// - setStats with statistics that do not match the table (wrong number
    ///   of columns, a min/max/lo/hi of the wrong type) → std::logic_error
    ///   (the caller built them wrongly, D1);
    /// - a corrupt catalog.db at load() → std::runtime_error naming the page
    ///   and slot.
    class Catalog
    {
    public:
        /// `dbDir` is the database folder; `catalogFile` is `catalog.db`,
        /// already open in `files`. Reads nothing (call load()).
        Catalog(std::filesystem::path dbDir, FileRegistry& files, BufferPool& pool, FileId catalogFile);

        Catalog(const Catalog&) = delete;
        Catalog& operator=(const Catalog&) = delete;
        Catalog(Catalog&&) = delete;
        Catalog& operator=(Catalog&&) = delete;

        /// Replaces the map with what `catalog.db` holds: every page through
        /// the pool, one page pinned at a time, every live slot. An empty
        /// file = no tables. Afterwards nothing counts as changed.
        void load();

        /// Writes the map to `catalog.db` if anything changed since the last
        /// load/save; otherwise does nothing (no I/O). Records go in a fixed
        /// order (table records by name, then each analyzed table's
        /// statistics chunks), filling pages from page 0; existing pages are
        /// reused and new ones added only when needed. Pages left over when
        /// the catalog got smaller are rewritten as empty pages (the file
        /// never shrinks). One page pinned at a time. The pages are left
        /// dirty in the pool; the engine's flushAll writes them.
        void save();

        /// True if the map differs from what was last loaded or saved.
        bool changed() const;

        /// Creates table `name` with `schema`: checks both, creates the
        /// empty heap file `<DIR>/<name>.db` (page 0 = its directory page)
        /// and adds the table with 0 pages and 0 tuples, no statistics.
        /// `schema` may be qualified (e.g. `R.a`); the qualifiers are dropped.
        /// Throws db::Error(Schema), and changes nothing, if:
        /// - the name is empty or has a character other than a letter,
        ///   digit or `_` (it becomes a file name, so `/`, `..`, spaces are
        ///   impossible): "Invalid table name 'a b' (use only letters, digits and _).";
        /// - the name is `catalog` in any case (it would be `catalog.db`);
        /// - the table exists: "Table 'R' already exists.";
        /// - the schema has no columns, or two columns with the same name
        ///   once qualifiers are dropped;
        /// - the table's catalog record would not fit on one page (4084 bytes).
        /// File errors as FileRegistry::create (e.g. `<name>.db` already on disk;
        /// on Windows that includes `emp` when `Emp` exists, DECISIONS 2026-10-30).
        void createTable(const std::string& name, const Schema& schema);

        /// A COPY of the table's entry, with the schema qualified by the
        /// table name (`R.a`, `R.b`); empty if there is no such table. Exact,
        /// case-sensitive lookup. No I/O.
        std::optional<TableInfo> getTable(const std::string& name) const;

        /// True if table `name` exists (exact, case-sensitive). No I/O.
        bool hasTable(const std::string& name) const;

        /// Every table name, sorted by byte order (`Emp, R, S, dept`). No I/O.
        std::vector<std::string> listTables() const;

        /// Sets the table's number of data pages and live tuples (called by
        /// whoever inserted rows, e.g. the loader). Unknown table → Error(Name).
        void updateCounts(const std::string& name, std::uint32_t numPages, std::uint64_t numTuples);

        /// Stores (or replaces) the table's statistics. Needs one ColumnStats
        /// per column, each min/max/lo/hi of that column's type, else
        /// std::logic_error. Unknown table → Error(Name).
        void setStats(const std::string& name, const TableStats& stats);

    private:
        friend class StorageEngine;
        /// StorageEngine::resetPool replaces the pool; this points the
        /// catalog at the new one.
        void usePool(BufferPool& pool);

        /// The entry of table `name`, or Error(Name) "Unknown table 'X'.".
        TableInfo& entry(const std::string& name);

        std::filesystem::path dbDir_;
        FileRegistry& files_;
        /// A pointer, not a reference, so usePool can repoint it.
        BufferPool* pool_;
        FileId catalogFile_;
        /// name -> entry, columns bare. A std::map, so it is always sorted
        /// by name: listTables and save need no sorting of their own.
        std::map<std::string, TableInfo> tables_;
        bool changed_ = false;
    };
} // namespace db::storage
