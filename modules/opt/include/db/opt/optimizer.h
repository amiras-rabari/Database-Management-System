#pragma once

/// @file
/// The whole optimizer in one call (§M4): a logical plan in, a physical
/// plan in exec's exact JSON out, with everything explain and the report
/// need on the way.
///
/// The steps, in order:
/// 1. Check the logical plan (common's outputSchema): an unknown table or
///    column fails here, before any work.
/// 2. Rewrite it (§M4.3, the rules switched on in the options).
/// 3. Find each join block: a group of joins and crosses next to each
///    other. Its relations are the subtrees below it (a scan with its
///    filters, or a derived table).
/// 4. Order each block's joins with the DP (§M4.5), or keep the written
///    order (unoptimized mode, or more than six relations).
/// 5. Rebuild the block as a left-deep tree in that order, put a project on
///    top when the column order changed (so `SELECT *` keeps the written
///    order), and run rule 6 again for the new join positions.
/// 6. Convert to a physical plan (chunk 10) and give every node its own
///    estimated cost with the frames exec really gives it (divideFrames).

#include <cstdint>
#include <vector>

#include "db/common/plan.h"
#include "db/opt/enumerate.h"
#include "db/opt/estimate.h"
#include "db/opt/physical.h"
#include "db/opt/rewrite.h"

namespace db::opt
{
    /// How to optimize.
    struct OptimizerOptions
    {
        /// B, the memory budget in pages (`--frames`, >= 3). The caller
        /// always sets it.
        std::int64_t frames = 0;
        /// Which rewrite rules are on (§M4.7 #3 turns them off one at a
        /// time). Ignored in unoptimized mode.
        RuleSet rules = kAllRules;
        /// The unoptimized plan of §M4.7 #2: "joins in written order, one
        /// fixed algorithm, no pushdown". Rules 2, 4, 5 and 6 are off (they
        /// push selects and projections down), rules 1, 3 and 7 stay on
        /// (they move nothing toward the leaves), there is no DP, and every
        /// join uses `fixedAlgorithm`.
        bool unoptimized = false;
        /// The one algorithm of unoptimized mode. Nested loop by default:
        /// it is the only one that can run every join; another one falls
        /// back to nested loop where it cannot run.
        JoinAlgorithm fixedAlgorithm = JoinAlgorithm::NestedLoop;
    };

    /// Everything the optimizer produced.
    struct OptimizedPlan
    {
        /// The logical plan as it came in.
        PlanNode logicalBefore;
        /// The logical plan after the rewrite rules, before join ordering:
        /// the "after" tree explain shows beside `logicalBefore` (§M4.3).
        PlanNode logicalAfter;
        /// The physical plan, ready for toJsonText and exec (§M4.6).
        PlanNode physical;
        /// One estimate per node of `physical`, in pre-order (the same
        /// index as exec's trace rows). `cost` is the node's own I/O,
        /// costed with the frames divideFrames gives it.
        std::vector<NodeEstimate> estimates;
        /// One DP table per join block of two or more relations, in the
        /// order they were planned: a block inside a derived table before
        /// the block that uses it, a left branch before a right one. A
        /// block of more than six relations keeps its written order; its
        /// table has the names and no entries. Empty in unoptimized mode.
        std::vector<DpTable> dpTables;
    };

    /// Optimizes `logical` with the statistics from `stats`.
    /// Errors: an invalid plan (unknown table or column, type error) throws
    /// common's Error before any work; a plan that needs more than B frames
    /// throws exec's Error(Schema) from divideFrames.
    OptimizedPlan optimize(const PlanNode& logical, const StatsProvider& stats, const OptimizerOptions& options);
} // namespace db::opt
