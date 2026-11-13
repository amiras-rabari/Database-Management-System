#include "db/opt/enumerate.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "db/common/condition.h"
#include "db/opt/cost.h"
#include "db/opt/estimate.h"
#include "db/opt/physical.h"

namespace db::opt
{
    namespace
    {
        // ---- Small helpers for groups stored as bitmasks ------------------
        // A group is a number whose bit i says whether relation i is in it.
        // Example with A = 0, B = 1, C = 2: {A, C} = binary 101 = 5.

        // The group holding only relation i: bit i set (1 << 2 = binary 100).
        std::uint32_t onlyRelation(std::size_t i)
        {
            return 1u << i;
        }

        // True when relation i is in `group` (its bit is 1).
        bool contains(std::uint32_t group, std::size_t i)
        {
            return (group & onlyRelation(i)) != 0;
        }

        // How many relations are in `group` (how many bits are 1).
        std::size_t memberCount(std::uint32_t group, std::size_t relationCount)
        {
            std::size_t count = 0;
            for (std::size_t i = 0; i < relationCount; ++i)
            {
                if (contains(group, i))
                {
                    ++count;
                }
            }
            return count;
        }

        // True when every member of `small` is also in `big`.
        bool isInside(std::uint32_t small, std::uint32_t big)
        {
            return (small & big) == small;
        }

        // ---- The join predicates ------------------------------------------

        // True when `predicate` links group `outer` and relation `inner`: it
        // uses `inner`, and every relation it uses is in `outer` or is
        // `inner`. A predicate uses at least two relations, so it then also
        // uses something in `outer`. Such a predicate can be checked exactly
        // at the join of `outer` with `inner`, and not earlier.
        // Example: `A.x = B.x` links {B} and A, but not {C} and A.
        // Example: Suppose the rule is A.x = B.x (it needs both A and B)
        //Joining outer = {B} with inner = A -> true joined becomes {A, B}.
        // usesInner: Does A.x = B.x use the new table A? Yes.
        // isInside: Are both A and B inside joined? Yes.
        // Result: true — we have both pieces, so we can check A.x = B.x right now.

        bool links(const JoinPredicate& predicate, std::uint32_t outer, std::size_t inner)
        {
            std::uint32_t joined = outer | onlyRelation(inner);
            bool usesInner = contains(predicate.relations, inner);
            return usesInner && isInside(predicate.relations, joined);
        }

        // True when the predicates connect all relations into one piece
        // (from relation 0 you can reach every other through predicates).
        // Start with relation 0, and keep adding the relations of any
        // predicate that touches what is reached so far.
        // Example: Relations A, B, C with predicates `A.x = B.x` and `B.y = C.y`.
        // - Start at relation 0: reached = {A}
        // - Pass 1:
        //     `A.x = B.x` touches {A} -> pulls in B -> reached = {A, B}
        //     `B.y = C.y` touches {A, B} -> pulls in C -> reached = {A, B, C}
        // - Pass 2: no new relations added -> grew is false, loop stops.
        // - Final check: reached == everyone ({A, B, C} == {A, B, C}) -> returns true.
        //   (If `B.y = C.y` were missing, C stays stranded, reached stops at {A, B},
        //    and it returns false, telling the optimizer a cross product is required.)

        bool allConnected(const JoinGraph& graph)
        {
            std::size_t n = graph.relations.size();
            std::uint32_t everyone = (1u << n) - 1;
            std::uint32_t reached = onlyRelation(0);
            bool grew = true;
            while (grew)
            {
                grew = false;
                for (const JoinPredicate& predicate : graph.predicates)
                {
                    bool touchesReached = (predicate.relations & reached) != 0;
                    if (touchesReached && !isInside(predicate.relations, reached))
                    {
                        reached = reached | predicate.relations;
                        grew = true;
                    }
                }
            }
            return reached == everyone;
        }

        // ---- Costs ----------------------------------------------------------

        // The join formula alone (§M4.4, chunk 09), for an outer of m pages
        // and an inner of n pages.
        std::int64_t formulaCost(JoinAlgorithm algorithm, std::int64_t m, std::int64_t n, std::int64_t frames)
        {
            if (algorithm == JoinAlgorithm::SortMerge)
            {
                return costSortMergeJoin(m, n, frames);
            }
            if (algorithm == JoinAlgorithm::Hash)
            {
                return costHashJoin(m, n);
            }
            return costNestedLoopJoin(m, n, frames);
        }

        // ---- The DP state ---------------------------------------------------

        // Everything the DP keeps while it runs.
        struct DpState
        {
            const JoinGraph& graph;
            const std::vector<NodeEstimate>& subsetSizes;
            std::int64_t frames;
            // The table being filled.
            DpTable table;
            // For each group (index = its mask): does it have a plan yet,
            // and where is its entry in table.entries.
            std::vector<bool> hasPlan;
            std::vector<std::size_t> position;
        };

        // Adds a finished entry to the table and remembers where it is.
        void addEntry(DpState& state, const DpEntry& entry)
        {
            state.position[entry.subset] = state.table.entries.size();
            state.hasPlan[entry.subset] = !entry.skipped;
            state.table.entries.push_back(entry);
        }

        // Tries every way to build `entry.subset` as (smaller group) join
        // (one relation), and adds each one to entry.candidates.
        // With needPredicate, pairings with no linking predicate are skipped
        // (§M4.5); without it they are tried as cross products.
        void tryEveryPairing(DpState& state, DpEntry& entry, bool needPredicate)
        {
            std::size_t n = state.graph.relations.size();
            for (std::size_t inner = 0; inner < n; ++inner)
            {
                if (!contains(entry.subset, inner))
                {
                    continue;
                }
                // The smaller group: this group without `inner` (clear its bit).
                std::uint32_t outer = entry.subset & ~onlyRelation(inner);
                if (!state.hasPlan[outer])
                {
                    continue; // e.g. {A,C} was skipped, so it cannot be an outer
                }

                // Which predicates link the two, and is one an equality?
                bool linked = false;
                bool hasEquality = false;
                for (const JoinPredicate& predicate : state.graph.predicates)
                {
                    if (links(predicate, outer, inner))
                    {
                        linked = true;
                        if (predicate.equality)
                        {
                            hasEquality = true;
                        }
                    }
                }
                if (needPredicate && !linked)
                {
                    continue; // the skip rule
                }

                // M = the outer's pages, N = the inner relation's pages.
                const DpEntry& outerEntry = state.table.entries[state.position[outer]];
                std::int64_t m = outerEntry.pages;
                std::int64_t nPages = state.graph.relations[inner].pages;
                // A base outer is read inside the formula (its M term);
                // a joined outer must also be built first (chunk 01
                // decision 2: 110 + [20 + 1 x 400] = 530).
                bool outerIsBase = memberCount(outer, n) == 1;
                std::int64_t outerCost = outerIsBase ? 0 : outerEntry.cost;

                // Nested loop always works. Sort-merge and hash need an
                // equality; the other linking predicates then go into a
                // filter above the join (chunk 10), which costs nothing.
                std::vector<JoinAlgorithm> algorithms;
                algorithms.push_back(JoinAlgorithm::NestedLoop);
                if (hasEquality)
                {
                    algorithms.push_back(JoinAlgorithm::SortMerge);
                    algorithms.push_back(JoinAlgorithm::Hash);
                }
                for (JoinAlgorithm algorithm : algorithms)
                {
                    std::int64_t cost = outerCost + formulaCost(algorithm, m, nPages, state.frames);
                    entry.candidates.push_back(DpCandidate{outer, inner, algorithm, cost});
                }
            }
        }

        // Builds the entry of a group of two or more relations.
        DpEntry solveGroup(DpState& state, std::uint32_t subset, bool crossAllowed)
        {
            DpEntry entry;
            entry.subset = subset;
            tryEveryPairing(state, entry, true);
            // "unless no alternative exists" (§M4.5): only when the
            // relations are not all connected, and only for a group no
            // predicate can build.
            if (entry.candidates.empty() && crossAllowed)
            {
                tryEveryPairing(state, entry, false);
            }
            if (entry.candidates.empty())
            {
                entry.skipped = true;
                return entry;
            }

            // The cheapest wins. "<" keeps the earlier one on a tie, so the
            // order of trying (lower inner first, then nested loop,
            // sort-merge, hash) breaks ties.
            entry.chosen = 0;
            for (std::size_t i = 1; i < entry.candidates.size(); ++i)
            {
                if (entry.candidates[i].cost < entry.candidates[entry.chosen].cost)
                {
                    entry.chosen = i;
                }
            }
            entry.cost = entry.candidates[entry.chosen].cost;
            entry.rows = state.subsetSizes[subset].rows;
            entry.pages = state.subsetSizes[subset].pages;
            return entry;
        }

        // Follows the winners down from the group of all relations and
        // writes the chosen order into state.table.first / steps.
        // Example (§M4.5): {A,B',C} was built as {A,B'} + C, and {A,B'} as
        // {B'} + A, so: first = B', steps = [join A, join C].
        void writeChosenOrder(DpState& state)
        {
            std::size_t n = state.graph.relations.size();
            std::uint32_t group = (1u << n) - 1;
            while (memberCount(group, n) > 1)
            {
                const DpEntry& entry = state.table.entries[state.position[group]];
                const DpCandidate& winner = entry.candidates[entry.chosen];
                JoinStep step;
                step.inner = winner.inner;
                step.algorithm = winner.algorithm;
                for (const JoinPredicate& predicate : state.graph.predicates)
                {
                    if (links(predicate, winner.outer, winner.inner))
                    {
                        step.conditions.push_back(predicate.condition);
                    }
                }
                // We walk from the top join down, so each step goes in front:
                // the bottom join ends up first.
                state.table.steps.insert(state.table.steps.begin(), step);
                group = winner.outer;
            }
            // One relation is left: the one the plan starts with.
            for (std::size_t i = 0; i < n; ++i)
            {
                if (contains(group, i))
                {
                    state.table.first = i;
                }
            }
        }
    } // namespace

    DpTable enumerateJoins(const JoinGraph& graph, const std::vector<NodeEstimate>& subsetSizes, std::int64_t frames)
    {
        std::size_t n = graph.relations.size();
        std::uint32_t everyone = (1u << n) - 1;

        DpState state{
            graph, subsetSizes, frames, DpTable{}, std::vector<bool>(everyone + 1, false),
            std::vector<std::size_t>(everyone + 1, 0)
        };
        for (const JoinRelation& relation : graph.relations)
        {
            state.table.names.push_back(relation.name);
        }

        // Step 1: every relation alone. Its cost is its access path
        // (§M4.5 "best[{r}]"), e.g. {A} = 100.
        for (std::size_t i = 0; i < n; ++i)
        {
            DpEntry entry;
            entry.subset = onlyRelation(i);
            entry.cost = graph.relations[i].cost;
            entry.rows = graph.relations[i].rows;
            entry.pages = graph.relations[i].pages;
            addEntry(state, entry);
        }

        // Step 2: groups of 2, then 3, ... relations. Every smaller group is
        // solved before any bigger one needs it. Within one size, groups are
        // visited in increasing mask order, so the table is the same on
        // every run.
        bool crossAllowed = !allConnected(graph);
        for (std::size_t size = 2; size <= n; ++size)
        {
            for (std::uint32_t subset = 1; subset <= everyone; ++subset)
            {
                if (memberCount(subset, n) == size)
                {
                    addEntry(state, solveGroup(state, subset, crossAllowed));
                }
            }
        }

        // Step 3: the answer is the group of all relations.
        writeChosenOrder(state);
        return state.table;
    }
} // namespace db::opt
