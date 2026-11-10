#pragma once

/// @file
/// Selectivity (§M4.2): the fraction of input rows a condition keeps, a
/// number in [0, 1]. The join row of §M4.2 (`R.a = S.b` across two inputs)
/// is not here: it needs two row counts and lives in the cardinality
/// estimator (chunk 05).
///
/// Formulas, for `column op literal`:
/// - `c = v`: with a histogram, find the bucket holding v:
///   count(bucket) / (n x ndv(bucket)), where n = the sum of the bucket
///   counts; 0 when no bucket holds v. Without a histogram: 1 / ndv(c).
/// - `c < v`, `c > v`: whole buckets on the correct side plus a linear
///   fraction of the bucket holding v ((v - lo) / (hi - lo) for <), over n.
///   Without a histogram, [min, max] is treated as one bucket (§M4.2 example:
///   age > 60 on 20..70 = 10 / 50). VARCHAR has no "v - lo": the bucket
///   holding v counts half.
/// - `c <= v` = sel(c < v) + sel(c = v); `c >= v` likewise (at most 1).
/// - `c <> v` = 1 - sel(c = v).
/// - `p AND q` = sel(p) x sel(q) (assumes independence); `p OR q` =
///   sel(p) + sel(q) - sel(p) x sel(q); `NOT p` = 1 - sel(p).
/// - Nothing known about c: the defaults in defaults.h (= 0.1, ranges 1/3,
///   <> 0.9). Column against column: 0.1. Literal against literal: 1 or 0.
/// No rounding here; rows are rounded once, in the cardinality estimator.

#include <functional>

#include "db/common/condition.h"
#include "db/common/schema.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    /// Finds the statistics of a column named in a condition, or returns
    /// nullptr when nothing is known (use the defaults). The cardinality
    /// estimator knows how names map through renames and joins; this file
    /// only asks.
    using ColumnStatsLookup = std::function<const storage::ColumnStats*(const ColumnRef&)>;

    /// The selectivity of `condition`, by the formulas above. Values of
    /// different types (INT against VARCHAR) -> Error(Type) from
    /// compareValues; sql and outputSchema reject such plans before opt.
    double estimateSelectivity(const Condition& condition, const ColumnStatsLookup& lookup);
} // namespace db::opt
