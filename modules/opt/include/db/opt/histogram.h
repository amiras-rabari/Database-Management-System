#pragma once

/// @file
/// Building one column's statistics from its values in sorted order (§M4.1):
/// ndv, min, max and an equi-depth histogram of at most k buckets.
///
/// Equi-depth means every bucket holds about n / k rows, so buckets are
/// narrow where the data is dense. §M4.1: "Build it by sorting the column and
/// cutting at every n/k-th value". ANALYZE (chunk 03) sorts the column with
/// exec's external sort and passes the sorted values in here one at a time.
///
/// Memory is O(k): only the finished buckets, the bucket being filled and
/// the previous value are kept, never the n values themselves. That is what
/// lets ANALYZE stay inside B frames on a table of any size.

#include <cstdint>
#include <functional>

#include "db/common/value.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    /// Reads the column's values from `next` and returns its statistics.
    ///
    /// - `type`: the column's type (INT or VARCHAR). Used only for an empty
    ///   column, whose min and max are placeholders of that type
    ///   (0 or the empty string), because storage refuses wrongly typed
    ///   values. Readers never see them: ndv is 0, so chunk 01's
    ///   `columnStats` returns nullptr.
    /// - `n`: how many values `next` will give. Knowing it up front fixes
    ///   every cut point before the first value arrives.
    /// - `buckets`: k, at least 1 (the CLI checks `--buckets`).
    /// - `next`: puts the next value in its argument and returns true, or
    ///   returns false when there are no more. The values must come in
    ///   non-decreasing order (compareValues), exactly n of them.
    ///
    /// Cutting rule: bucket i (from 0) ends after position (i + 1) * n / k,
    /// in whole numbers, except that a run of equal values is never split:
    /// the bucket stays open until the value changes. So buckets never
    /// overlap, a value lies in exactly one bucket, and a very common value
    /// gets a bucket of its own. Fewer than k buckets can result; none is
    /// empty.
    storage::ColumnStats buildColumnStats(Type type, std::uint64_t n, std::int32_t buckets,
                                          const std::function<bool(Value&)>& next);
} // namespace db::opt
