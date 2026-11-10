#pragma once

/// @file
/// ANALYZE (§M4.1): exact statistics of one table, stored in the M1 catalog.
///
/// Per table: n_tuples, n_pages (data pages only) and width (average record
/// length). Per column: ndv, min, max and an equi-depth histogram. Every
/// column is sorted with exec's external sort (§M4.1: "The sort must use your
/// external sort from Module 2"), so ANALYZE works on a table of any size
/// inside the B frames of the engine's buffer pool.

#include <cstdint>
#include <string>

#include "db/storage/storage_engine.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    /// Computes the statistics of `table`, stores them with
    /// Catalog::setStats (replacing any earlier ones) and returns them.
    ///
    /// - B is the pool's frame count; it must be at least 3 (an external sort
    ///   needs 3; the CLI checks `--frames`). `buckets` (k) must be at least
    ///   1 (the CLI checks `--buckets`).
    /// - Scans: one scan feeds the sorts of up to (B - 1) / 2 columns at
    ///   once (each sort needs 2 frames, the scan 1). At B = 32 a table of up
    ///   to 15 columns is scanned once, as §M4.1 says; at B = 4 each column
    ///   gets its own scan, because two sorts do not fit.
    /// - Nothing else may hold frames while it runs (ANALYZE is a command on
    ///   its own). It never holds more than B frames, and on return or throw
    ///   it holds no page and has deleted every temporary file.
    /// - Running it twice gives identical statistics: it only reads the
    ///   table and stores nothing that changes between runs.
    /// - Unknown table -> Error(Name) "Unknown table 'R'.", before any work.
    storage::TableStats analyzeTable(storage::StorageEngine& engine, const std::string& table, std::int32_t buckets);
} // namespace db::opt
