#pragma once

/// @file
/// The statistics of one table (S11), chunk 11. Module 4 (opt) fills them
/// with ANALYZE and reads them for its cost model; storage only stores them
/// in `catalog.db` (DECISIONS 2026-10-27). They live here, not in opt,
/// because storage must encode them and opt depends on storage, never the
/// other way around.
///
/// Plain structs (aggregates): public fields, no rules to protect, copyable
/// values, and `==` for tests.

#include <cstdint>
#include <vector>

#include "db/common/value.h"

namespace db::storage
{
    /// One histogram bucket: the values in [lo, hi] of one column.
    struct HistogramBucket
    {
        /// Smallest and largest value in the bucket. Both have the column's
        /// type (INT or VARCHAR).
        Value lo = Value::ofInt(0);
        Value hi = Value::ofInt(0);
        /// How many rows fall in the bucket.
        std::uint64_t count = 0;
        /// How many distinct values fall in the bucket.
        std::uint64_t ndv = 0;

        bool operator==(const HistogramBucket&) const = default;
    };

    /// The statistics of one column.
    struct ColumnStats
    {
        /// Number of distinct values in the column.
        std::uint64_t ndv = 0;
        /// Smallest and largest value, of the column's type.
        Value min = Value::ofInt(0);
        Value max = Value::ofInt(0);
        /// Buckets in ascending order (opt chooses how many; default 20).
        /// Empty is allowed.
        std::vector<HistogramBucket> histogram;

        bool operator==(const ColumnStats&) const = default;
    };

    /// The statistics of one table.
    struct TableStats
    {
        /// Number of rows when ANALYZE ran.
        std::uint64_t nTuples = 0;
        /// Number of data pages (directory pages not counted, S11).
        std::uint32_t nPages = 0;
        /// Average record length in bytes. Stored on disk as its 8 raw
        /// IEEE-754 bytes, so it reads back bit for bit (DECISIONS 2026-10-27).
        double width = 0.0;
        /// One entry per column, in schema order.
        std::vector<ColumnStats> columns;

        bool operator==(const TableStats&) const = default;
    };
} // namespace db::storage
