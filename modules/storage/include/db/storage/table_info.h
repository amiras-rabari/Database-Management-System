#pragma once

/// @file
/// Everything the catalog knows about one table (§M1.7, conflict #14),
/// chunk 11: name, columns (type and length), data file name, first page,
/// number of data pages, number of live tuples, and the statistics M4 adds.

#include <cstdint>
#include <optional>
#include <string>

#include "db/common/schema.h"
#include "db/storage/page.h"
#include "db/storage/table_stats.h"

namespace db::storage
{
    /// A plain value. Catalog::getTable returns a copy, so the caller can keep
    /// it as long as it likes and cannot change the catalog through it.
    struct TableInfo
    {
        /// The table name, e.g. `R` (case-sensitive).
        std::string name;
        /// The columns in order. Inside the catalog (and on disk) the names
        /// are bare (`a`); Catalog::getTable returns them qualified with the
        /// table name (`R.a`).
        Schema schema;
        /// The table's file in the database folder: always `<name>.db`.
        std::string dataFile;
        /// The heap file's first page: always 0 (its first directory page).
        PageId firstPage = 0;
        /// Number of DATA pages (directory pages not counted, S11). Set by
        /// whoever adds rows, through Catalog::updateCounts.
        std::uint32_t numPages = 0;
        /// Number of live rows, also set through Catalog::updateCounts.
        std::uint64_t numTuples = 0;
        /// Statistics; empty = never analyzed (opt then uses its defaults).
        std::optional<TableStats> stats;
    };

    /// True if the two schemas have the same columns in the same order
    /// (qualifier, name, type and VARCHAR length). it doesn't look for duplicates,
    /// it checks whether two whole schemas are equal.
    /// It exists only because common's Schema has no == built in,
    /// and storage may not edit common. It's used just for TableInfo == in tests.
    inline bool sameColumns(const Schema& a, const Schema& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const Column& x = a.column(i);
            const Column& y = b.column(i);
            if (x.qualifier != y.qualifier || x.name != y.name || x.type != y.type
                || x.varcharLength != y.varcharLength)
                return false;
        }
        return true;
    }

    /// Field-by-field equality, for tests. Written by hand (not `= default`)
    /// only because Schema has no `==` same reason as above.
    inline bool operator==(const TableInfo& a, const TableInfo& b)
    {
        return a.name == b.name && sameColumns(a.schema, b.schema) && a.dataFile == b.dataFile
            && a.firstPage == b.firstPage && a.numPages == b.numPages && a.numTuples == b.numTuples
            && a.stats == b.stats;
    }
} // namespace db::storage
