#pragma once

/// @file
/// The cardinality estimator (§M4.2): for every node of a logical plan, how
/// many rows it outputs, how wide a row is and how many pages that makes.
///
/// The two rules every node follows (§M4.2):
/// - rows = input rows x selectivity, rounded up, never below 1;
/// - pages = ceil(rows x width / 4096), never below 1.
/// A projection that drops columns makes rows narrower, so it lowers the
/// page count of everything above it. That is how rule 6's projections make
/// later joins cheaper.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    /// The estimate of one plan node.
    /// Counts are int64_t: a cross of two 100,000-row tables has 10^10 rows,
    /// far more than int32_t holds.
    struct NodeEstimate
    {
        /// Estimated output rows (at least 1).
        std::int64_t rows = 0;
        /// Estimated output pages: ceil(rows x width / 4096), at least 1.
        std::int64_t pages = 0;
        /// Average output row width in bytes.
        double width = 0.0;
        /// Estimated page I/O. Always 0 here; the cost model (chunks 09 and
        /// 12) fills it in.
        std::int64_t cost = 0;

        friend bool operator==(const NodeEstimate&, const NodeEstimate&) = default;
    };

    /// What is known about one output column of a node. A node keeps one per
    /// output column, in schema order, so a condition far above a rename or
    /// a join still finds the base column's statistics by position.
    struct ColumnFacts
    {
        /// ANALYZE's statistics of the base column this output column comes
        /// from, or nullptr (never analyzed, or a computed column such as
        /// count(*)). Points into a TableInfo, which must outlive it.
        const storage::ColumnStats* stats = nullptr;
        /// Estimated number of distinct values at this node; 0 = unknown.
        /// Never more than the node's rows.
        std::int64_t ndv = 0;
        /// Estimated width in bytes (statistics.h columnWidths).
        double width = 0.0;
    };

    /// Gives a table's catalog entry by name, or std::nullopt when there is
    /// no such table. The CLI wraps Catalog::getTable; tests return
    /// TableInfos built in memory (the spec's made-up Emp and Dept).
    using StatsProvider = std::function<std::optional<storage::TableInfo>(const std::string&)>;

    /// The smallest whole number >= x, and at least 1 (§M4.2 "rounded up,
    /// never below 1"). A tiny tolerance (1e-9 x max(1, |x|)) is taken off
    /// first: 0.1 x 3 x 10 is 3.0000000000000004 in binary floating point,
    /// and a plain ceil would make that 4.
    std::int64_t ceilRows(double x);

    /// ceil(rows x width / 4096), and at least 1 (§M4.2), with the same
    /// tolerance as ceilRows.
    std::int64_t ceilPages(std::int64_t rows, double width);

    /// The selectivity of ONE conjunct of a join condition between `left`
    /// and `right`. `facts` covers the left columns, then the right ones
    /// (the join's output order).
    /// - `L.a = R.b` across the two sides: 1 / max(ndv(L.a), ndv(R.b)), so
    ///   rows = n(L) x n(R) / max(...) as in §M4.2. If only one ndv is known
    ///   it is used alone; if neither, the default equality 0.1.
    /// - Anything else goes to estimateSelectivity: a column-against-column
    ///   predicate gets 0.1 (§M4.2 "any other join predicate"), and a
    ///   predicate on one side only (`e.age > 60` written in an ON) gets its
    ///   normal selectivity.
    double joinConjunctSelectivity(const Condition& conjunct, const Schema& left, const Schema& right,
                                   const std::vector<ColumnFacts>& facts);

    /// The estimate of every node of a logical plan, in PRE-ORDER: the root,
    /// then its left (or only) child's subtree, then its right child's
    /// subtree. Exec's trace rows use the same order, so entry i pairs with
    /// trace row i.
    ///
    /// The plan must be valid (outputSchema accepts it). A table the
    /// provider does not know -> Error(Name) "Unknown table 'R'.".
    std::vector<NodeEstimate> estimatePlan(const PlanNode& plan, const StatsProvider& stats);
} // namespace db::opt
