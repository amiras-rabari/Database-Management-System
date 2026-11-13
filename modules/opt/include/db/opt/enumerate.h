#pragma once

/// @file
/// Join ordering (§M4.5): Selinger-style dynamic programming over left-deep
/// trees, for up to six relations.
///
/// The idea in one paragraph: first write down the cost of every relation
/// alone. Then, for every group of 2 relations, then 3, and so on, find the
/// cheapest way to build that group as "a smaller group that is already
/// solved" joined with "one more base relation", trying every relation as
/// the one added last and every join algorithm. Because the smaller groups
/// were solved first, each step only looks up their best costs. The answer
/// is the entry for the group of all relations.
///
/// Groups are bitmasks: relation i is bit i, so with A = 0, B = 1, C = 2
/// the group {A, C} is binary 101 = 5. Six relations give at most
/// 2^6 = 64 groups, so a plain vector indexed by the mask holds them all.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "db/common/condition.h"
#include "db/opt/estimate.h"
#include "db/opt/physical.h"

namespace db::opt
{
    /// One relation of a join block: a leaf of the plan (a scan, possibly
    /// with its filters and projections, or a derived table).
    struct JoinRelation
    {
        /// Shown in the DP table, e.g. `A` or `e`.
        std::string name;
        /// The I/O to produce it on its own (§M4.5 "best[{r}] = the cheapest
        /// access path"), e.g. 100 to scan a 100-page table.
        std::int64_t cost = 0;
        /// Its estimated output pages: the M or N of a join formula.
        std::int64_t pages = 0;
        /// Its estimated output rows.
        std::int64_t rows = 0;
    };

    /// One conjunct of the join conditions, e.g. `A.x = B.x`.
    struct JoinPredicate
    {
        Condition condition;
        /// The relations it uses, as a bitmask (bit i = relation i). Always
        /// at least two relations: a condition on one relation belongs in
        /// that relation's leaf, not here.
        std::uint32_t relations = 0;
        /// True when sort-merge and hash join can run it: a column = column
        /// equality between two relations, same type on both sides (the
        /// caller decides it with mergeOrHashValid).
        bool equality = false;
    };

    /// The input of the DP: the relations in written order (relation i =
    /// bit i) and every join predicate between them.
    struct JoinGraph
    {
        std::vector<JoinRelation> relations;
        std::vector<JoinPredicate> predicates;
    };

    /// One way to build a group: `outer` (a smaller group, the left input)
    /// joined with base relation `inner` (the right input) by `algorithm`.
    struct DpCandidate
    {
        /// The smaller group, as a bitmask.
        std::uint32_t outer = 0;
        /// The index of the relation added last.
        std::size_t inner = 0;
        JoinAlgorithm algorithm = JoinAlgorithm::NestedLoop;
        /// The total I/O of building the whole group this way.
        std::int64_t cost = 0;

        friend bool operator==(const DpCandidate&, const DpCandidate&) = default;
    };

    /// One row of the DP table: one group of relations.
    struct DpEntry
    {
        /// The group, as a bitmask.
        std::uint32_t subset = 0;
        /// True when the group has no plan: no pairing inside it has a join
        /// predicate (`{A,C} no predicate, skipped` in §M4.5).
        bool skipped = false;
        /// The best total I/O (a single relation: its own cost).
        std::int64_t cost = 0;
        /// The group's estimated output size.
        std::int64_t rows = 0;
        std::int64_t pages = 0;
        /// Every way tried, in the order tried (empty for a single relation
        /// or a skipped group), and the position of the cheapest.
        std::vector<DpCandidate> candidates;
        std::size_t chosen = 0;

        friend bool operator==(const DpEntry&, const DpEntry&) = default;
    };

    /// One join of the chosen order: the plan so far (left) joined with
    /// relation `inner` (right).
    struct JoinStep
    {
        std::size_t inner = 0;
        JoinAlgorithm algorithm = JoinAlgorithm::NestedLoop;
        /// The predicates that become usable at exactly this join (all
        /// their relations are now in), in predicate order. Empty = a cross
        /// product.
        std::vector<Condition> conditions;

        friend bool operator==(const JoinStep&, const JoinStep&) = default;
    };

    /// The result: the whole DP table and the chosen order.
    struct DpTable
    {
        /// The relations' names, by index.
        std::vector<std::string> names;
        /// One entry per group: the single relations in index order, then
        /// the groups of 2, 3, ... relations, each size in increasing mask
        /// order.
        std::vector<DpEntry> entries;
        /// The chosen order: start with relation `first`, then apply the
        /// steps from the first to the last (the bottom join first).
        std::size_t first = 0;
        std::vector<JoinStep> steps;

        friend bool operator==(const DpTable&, const DpTable&) = default;
    };

    /// Runs the DP over `graph` (1 to 6 relations) with B = `frames` for
    /// every join (chunk 01 decision 5: the §M4.5 table gives each join the
    /// full B).
    ///
    /// `subsetSizes[mask]` is the estimated size (rows and pages) of the
    /// group `mask` once joined; it must have 2^n entries, and only those of
    /// groups with two or more relations are read. The caller computes them
    /// (a test can pass the spec's numbers directly).
    ///
    /// Costs (chunk 01 decision 2, so the §M4.5 table reproduces): a join
    /// formula already reads each input once, so a base outer adds nothing
    /// more (B' outer, A inner: 10 + 1 x 100 = 110), while a joined outer
    /// adds the cost of building it (110 + [20 + 1 x 400] = 530). Formulas:
    /// nested loop M + ceil(M/(B-2)) x N; sort-merge sort(M) + sort(N)
    /// (only with an equality); hash 3(M + N) (only with an equality).
    ///
    /// Skip rule (§M4.5): a pairing with no predicate between the smaller
    /// group and the added relation is not tried. If the relations are not
    /// all connected by predicates, a group with no such pairing at all is
    /// built from cross products instead (nested loop only), so the whole
    /// set always gets a plan.
    ///
    /// Ties: the candidate tried first wins, i.e. the lower added relation,
    /// then nested loop, sort-merge, hash.
    DpTable enumerateJoins(const JoinGraph& graph, const std::vector<NodeEstimate>& subsetSizes, std::int64_t frames);
} // namespace db::opt
