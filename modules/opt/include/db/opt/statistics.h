#pragma once

/// @file
/// Reading one table's statistics (§M4.1). Opt uses storage's own TableInfo
/// directly: it already holds the catalog counts and, after ANALYZE, the
/// TableStats. These functions answer the questions TableInfo cannot answer
/// in one field: how wide is a row, how wide is each column, and what is
/// known about one column.
///
/// Rows and pages: read `info.numTuples` and `info.numPages` directly. Never
/// use `stats->nTuples` / `stats->nPages` for estimates: storage keeps the
/// catalog counts exact on every insert and load, while the stats copy is the
/// count at the last ANALYZE and goes stale after later inserts. Right after
/// ANALYZE both are equal. `numPages` counts data pages only (§M4.1).
///
/// A test fakes a table by filling a TableInfo in memory, so the spec's
/// made-up tables (Emp with 10,000 rows on 200 pages) need no disk file.

#include <vector>

#include "db/common/schema.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    /// The widest record the schema allows: INT 4 bytes, VARCHAR(n) a 2-byte
    /// length plus n bytes. The width of an empty table, which has no rows
    /// to average (tableWidth below, and ANALYZE on an empty table).
    double schemaWidth(const Schema& schema);

    /// Average row width in bytes.
    /// - Analyzed: ANALYZE's exact width.
    /// - Not analyzed, with rows: pages x 4096 / rows, so an unfiltered scan
    ///   estimates exactly the table's real page count (10 pages, 1,000 rows
    ///   -> 40.96 bytes).
    /// - Not analyzed and empty: the widest record the schema allows
    ///   (INT 4 bytes, VARCHAR(n) 2 + n bytes), a finite number for a table
    ///   whose estimates are clamped to 1 row anyway.
    double tableWidth(const storage::TableInfo& info);

    /// The width of each column, in schema order. Storage keeps no
    /// per-column width, so it is derived from the table width: every INT
    /// column is 4 bytes, and the VARCHAR columns share the rest equally
    /// (Emp with width 30: four INTs take 16, name gets 14). With no VARCHAR
    /// column every column is 4 bytes. A projection uses these widths to
    /// lower its page estimate (§M4.2).
    std::vector<double> columnWidths(const storage::TableInfo& info);

    /// ANALYZE's statistics for one column, or nullptr when nothing is known
    /// (the table was never analyzed, or the column has ndv 0 because the
    /// table was empty). Callers then use the defaults in defaults.h.
    /// `ref` may be qualified (`Emp.age`) or bare (`age`); unknown column ->
    /// Error(Name), a bare name on several columns -> Error(Ambiguity).
    /// The pointer points into `info` and is valid only while `info` lives.
    const storage::ColumnStats* columnStats(const storage::TableInfo& info, const ColumnRef& ref);
} // namespace db::opt
