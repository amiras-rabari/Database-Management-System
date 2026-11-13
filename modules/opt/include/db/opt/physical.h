#pragma once

/// @file
/// Turns a logical plan (join order already fixed) into a physical plan in
/// the exact JSON exec reads (§M4.6: "valid input to your Module 2 executor
/// with no editing"), and estimates every physical node.
///
/// Node by node:
/// - scan -> seqscan, select -> filter; project, rename, sort, limit and the
///   set operations keep their names (rename is our physical extension,
///   DECISIONS 2026-11-01).
/// - join -> the cheapest valid one of the three joins (§M4.5 "for each
///   join, choose among your three algorithms"); cross -> nested_loop_join
///   with the condition `1 = 1` (exec needs a condition on every join).
/// - distinct, group_by -> sort or hash mode, whichever costs less.
/// - aggregates lose their alias (physical `[fn, col]`); every reference to
///   an alias above the group_by is renamed to the generated name
///   (`n` -> `count_star`, `t.n` -> `t.count_star`, DECISIONS 2026-11-07).
/// The left and right inputs of a join are never swapped: hash_join's left
/// input is the build side, nested_loop_join's the outer side.

#include <cstdint>
#include <optional>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/opt/estimate.h"

namespace db::opt
{
    /// The three join algorithms. The order matters: when two cost the same,
    /// the one listed first is chosen, so plans never depend on chance.
    enum class JoinAlgorithm
    {
        NestedLoop,
        SortMerge,
        Hash
    };

    /// True when sort-merge and hash join can run `condition` between inputs
    /// with schemas `left` and `right`: every conjunct is a column = column
    /// equality with one column on each side (common's equiJoinKeys), and
    /// both columns of each pair have the same type. Exec rejects anything
    /// else for these two joins (exec INTERFACE.md); nested loop accepts any
    /// condition. `R.a < S.c` or `R.b = 7` -> false.
    bool mergeOrHashValid(const Condition& condition, const Schema& left, const Schema& right);

    /// A physical plan and its estimates.
    struct PhysicalResult
    {
        /// The plan, ready for toJsonText and exec.
        PlanNode plan;
        /// One estimate per node of `plan`, in PRE-ORDER (the node, then its
        /// left subtree, then its right subtree): the same index as exec's
        /// trace rows. Kept outside the plan because the JSON reader rejects
        /// unknown fields. `cost` is 0 here; the planner fills it.
        std::vector<NodeEstimate> estimates;
    };

    /// Converts the logical plan `logical` (must be valid: outputSchema
    /// accepts it) for a run with B = `frames` (>= 3).
    /// - A join condition that mixes equalities usable by sort-merge/hash
    ///   with other conjuncts is split first: the equalities stay on the
    ///   join, the rest becomes a filter directly above it (settled
    ///   2026-11-07). The estimates are made on that split tree, so the
    ///   filter gets its own estimate.
    /// - Join: with `fixedAlgorithm` (the unoptimized plan, §M4.7 #2), that
    ///   algorithm if valid, else nested loop. Otherwise the cheapest valid
    ///   one at the full B (chunk 01 decision 5), costed with the input
    ///   pages: nested loop M + ceil(M/(B-2)) x N, sort-merge sort(M) +
    ///   sort(N), hash 3(M + N).
    /// - distinct/group_by: sort costs sort(M); hash costs costHashGroup
    ///   with a table of B - 1 frames (one frame is the input's page, exec's
    ///   phase table). Hash only when strictly cheaper.
    /// A table `stats` does not know -> Error(Name).
    PhysicalResult toPhysical(const PlanNode& logical, const StatsProvider& stats, std::int64_t frames,
                              std::optional<JoinAlgorithm> fixedAlgorithm = std::nullopt);
} // namespace db::opt
