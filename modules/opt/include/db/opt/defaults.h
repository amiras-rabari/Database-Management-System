#pragma once

/// @file
/// Every default number the optimizer uses, in one place, so DESIGN_M4.md can
/// list them (§M4.9: "a plan with no statistics available still produces a
/// valid plan, using documented defaults").
///
/// The selectivity guesses are used only when a column has no statistics
/// (the table was never analyzed, or it was empty). Row and page counts never
/// need a default: the catalog keeps them exact without ANALYZE. Decided
/// 2026-11-09, option (a): the classic System R guesses.

#include <cstdint>

namespace db::opt
{
    /// `c = v` with nothing known about c: keeps 1 row in 10.
    inline constexpr double kDefaultEqualitySelectivity = 0.1;

    /// `c < v`, `c > v`, `c <= v`, `c >= v` with nothing known: keeps 1 row
    /// in 3.
    inline constexpr double kDefaultRangeSelectivity = 1.0 / 3.0;

    /// `c <> v` with nothing known: 1 - 0.1, the opposite of equality.
    inline constexpr double kDefaultNotEqualSelectivity = 0.9;

    /// Any join predicate that is not `R.a = S.b` (§M4.2 requires 0.1).
    inline constexpr double kDefaultOtherJoinSelectivity = 0.1;

    /// Histogram buckets per column when `--buckets` is not given (§M4.1).
    inline constexpr std::int32_t kDefaultBuckets = 20;
} // namespace db::opt
