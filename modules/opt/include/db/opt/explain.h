#pragma once

/// @file
/// The two texts a reader of the optimizer looks at (§M4.6, §M4.5): the
/// `explain` tree with an estimate on every line, and the DP table. Both
/// are returned as strings; the library never prints (the `opt` tool and
/// the shell do).

#include <string>
#include <vector>

#include "db/common/plan.h"
#include "db/opt/enumerate.h"
#include "db/opt/estimate.h"

namespace db::opt
{
    /// One line per node of `plan`, in pre-order (the node, then its left
    /// subtree, then its right subtree), indented 2 spaces per level. Line i
    /// shows `estimates[i]`, so `estimates` must be in the same pre-order
    /// (estimatePlan and OptimizedPlan::estimates are). Exec's trace rows
    /// use the same order, so line i, estimates[i] and trace row i are the
    /// same node.
    ///
    /// Each line is the node's label, then `est_rows=N`, then (when
    /// `showCost`) `est_cost=C`, lined up in columns (§M4.6):
    /// ```
    /// hash_join (R.b = S.b)        est_rows=2000   est_cost=330
    ///   filter (R.a > 500)         est_rows=1000   est_cost=0
    ///     seqscan R                est_rows=10000  est_cost=200
    ///   seqscan S                  est_rows=500    est_cost=10
    /// ```
    /// `est_rows=` starts 8 spaces after the longest label; `est_cost=`
    /// starts 2 spaces after the longest `est_rows=N`. Every line ends with
    /// "\n".
    ///
    /// Use `showCost = true` for a physical plan (each node's own cost,
    /// OptimizedPlan::estimates). Use `false` for the logical trees before
    /// and after the rewrite rules (§M4.3): a logical join has no algorithm
    /// yet, so it has no cost (§M4.4 costs physical operators only).
    ///
    /// Labels: `seqscan R`, `filter (R.a > 500)`, `project [R.a, R.b]`,
    /// `hash_join (R.b = S.b)`, `sort [R.a asc]`,
    /// `group_by [R.dept; count(*), avg(R.salary)] mode=hash`,
    /// `distinct mode=sort`, `limit 10`, `rename e`, `union`. Logical nodes
    /// use the same pattern: `scan Emp`, `select (e.age > 60)`,
    /// `join (e.did = d.did)`, `cross`, `count(*) as n`. Conditions are
    /// written exactly as in plan JSON (common's toString).
    std::string explainText(const PlanNode& plan, const std::vector<NodeEstimate>& estimates, bool showCost);

    /// The DP table of one join block as a Markdown table, one row per
    /// group of relations (§M4.5), headed by a line naming the relations:
    /// ```
    /// Join block: A, B', C
    ///
    /// | Subset | Best plan considered | Cost |
    /// |---|---|---|
    /// | {A} | scan A | 100 |
    /// | {A,B'} | B' outer, BNL with A inner | 110 |
    /// | {A,C} | no predicate, skipped | — |
    /// | {A,B',C} | {A,B'} outer (20 pages), BNL with C inner | 530 |
    /// | | alternative: {B',C} outer (40 pages), BNL with A inner | 650 |
    /// ```
    /// Rows come in the DP's order (single relations, then groups of 2, 3,
    /// ...). The algorithm is `BNL`, `sort-merge` or `hash`. For the group
    /// of all relations, an `alternative` row follows for every other
    /// relation that could have been joined last: its cheapest way.
    /// A block of more than six relations (no entries) gets one line saying
    /// its written order was kept.
    std::string dpTableText(const DpTable& table);
} // namespace db::opt
