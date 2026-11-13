#include "db/opt/optimizer.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/exec/frame_plan.h"
#include "db/opt/cost.h"
#include "db/opt/enumerate.h"
#include "db/opt/estimate.h"
#include "db/opt/physical.h"
#include "db/opt/rewrite.h"
#include "db/storage/table_info.h"

namespace db::opt
{
    namespace
    {
        // §M4.5: the DP orders at most six relations. A bigger block keeps
        // its written order (settled 2026-11-07).
        constexpr std::size_t kMaxDpRelations = 6;

        // ceil(a / b) for positive whole numbers, without floating point.
        std::int64_t ceilDivide(std::int64_t a, std::int64_t b)
        {
            return (a + b - 1) / b;
        }

        // -------------------------------------------------------------------
        // Per-node cost with the frames exec gives each node (decision 5,
        // chunk 12 Q2 (b)).

        // The I/O of an external sort of m pages, counted the way exec's
        // sorter runs it (exec INTERFACE.md, "Variant: rewrite"):
        // - pass 0 fills `runFrames` pages at a time: ceil(m / runFrames) runs;
        // - each merge pass merges (mergeFrames - 1) runs into one;
        // - merging stops when the final merge can read every run left at
        //   once, i.e. at most (finalFrames - 1) runs;
        // - the final merge streams to the parent and writes nothing.
        // Every pass but the last reads and writes all m pages, so the cost
        // is §M4.4's 2 x m x (passes - 1) + m. Only the pass count differs
        // from the plain formula: the plain one uses one B for all three.
        // Example: 12 pages, runs of 3, merges and final of 4: 4 runs, one
        // merge pass to 2 runs, the final merge: 3 passes, 60 (formula: 36).
        // If the input fits in the run buffer and in the final merge, exec
        // sorts it in memory and writes nothing: m. `alwaysWrite` is for the
        // two sorts of a sort-merge join, intersect or except, which always
        // write their last run (exec's finishToRuns).
        std::int64_t sortCost(std::int64_t m, std::int64_t runFrames, std::int64_t mergeFrames,
                              std::int64_t finalFrames, bool alwaysWrite)
        {
            if (!alwaysWrite && m <= runFrames && m <= finalFrames)
            {
                return m;
            }
            std::int64_t runs = ceilDivide(m, runFrames);
            std::int64_t passes = 1;
            while (runs > finalFrames - 1)
            {
                runs = ceilDivide(runs, mergeFrames - 1);
                ++passes;
            }
            // The final merge.
            ++passes;
            return 2 * m * (passes - 1) + m;
        }

        // The two sorts of a sort-merge join, intersect or except (exec,
        // DESIGN_M2 §8): each makes runs with the node's own frames and
        // merges with its open frames; then both final merges stream side
        // by side and share `finalReaders` frames, one per run. The left
        // sort gets the larger half when the number is odd: 5 frames give
        // the left 3 runs and the right 2. Exec's interface does not state
        // this split; it was measured (12-page inputs at B = 3 to 8, sort-
        // merge, intersect and except, checked in planner_tests.cpp).
        std::int64_t twoSortsCost(std::int64_t m, std::int64_t n, const exec::FrameShare& share,
                                  std::int64_t finalReaders)
        {
            auto own = static_cast<std::int64_t>(share.ownFrames);
            auto open = static_cast<std::int64_t>(share.openFrames);
            // sortCost stops merging at (finalFrames - 1) runs, so a final
            // merge that reads r runs at once has finalFrames = r + 1.
            std::int64_t leftReaders = (finalReaders + 1) / 2;
            std::int64_t rightReaders = finalReaders / 2;
            return sortCost(m, own, open, leftReaders + 1, true) + sortCost(n, own, open, rightReaders + 1, true);
        }

        // The I/O of one physical node, with `share` = its frames from
        // divideFrames. `self` and `right` are the pre-order indexes of the
        // node and of its right child; the left (or only) child is self + 1.
        // Like §M4.4 (decision 2 of chunk 01), each formula includes one read
        // of the node's inputs.
        std::int64_t nodeCost(const PlanNode& node, const exec::FrameShare& share,
                              const std::vector<NodeEstimate>& estimates, std::size_t self, std::size_t right)
        {
            auto own = static_cast<std::int64_t>(share.ownFrames);
            auto open = static_cast<std::int64_t>(share.openFrames);
            auto stream = static_cast<std::int64_t>(share.streamFrames);
            // M and N: the estimated pages of the left and right inputs.
            std::int64_t m = node.children.empty() ? 0 : estimates[self + 1].pages;
            std::int64_t n = node.children.size() < 2 ? 0 : estimates[right].pages;
            switch (node.op)
            {
            case PlanOp::SeqScan:
                return estimates[self].pages;
            case PlanOp::Sort:
                return sortCost(m, own, open, stream, false);
            case PlanOp::Distinct:
            case PlanOp::GroupBy:
                if (node.mode == ExecMode::Hash)
                {
                    // The hash table gets the node's own frames.
                    return costHashGroup(m, estimates[self].pages, own);
                }
                if (node.op == PlanOp::GroupBy && node.columns.empty())
                {
                    // No grouping keys: one group, exec does not sort.
                    return m;
                }
                return sortCost(m, own, open, stream, false);
            case PlanOp::Union:
                // One sort of both inputs (DECISIONS 2026-11-07).
                return sortCost(m + n, own, open, stream, false);
            case PlanOp::Intersect:
            case PlanOp::Except:
                return twoSortsCost(m, n, share, stream);
            case PlanOp::SortMergeJoin:
                // One stream frame holds the run of equal right keys.
                return twoSortsCost(m, n, share, stream - 1);
            case PlanOp::HashJoin:
                return costHashJoin(m, n);
            case PlanOp::NestedLoopJoin:
                {
                    // The block is the node's own frames: B - 2 in §M4.4's
                    // formula, so B = block + 2.
                    std::int64_t cost = costNestedLoopJoin(m, n, own + 2);
                    // An input that does not fit beside the block is written to
                    // a run once (exec's writeOuter / writeInner); the formula
                    // already counts reading it.
                    if (share.writeOuter)
                    {
                        cost += m;
                    }
                    if (share.writeInner)
                    {
                        cost += n;
                    }
                    return cost;
                }
            case PlanOp::UnionAll:
                // Pipelined, unless its right input is written to a run and
                // read back (exec's writeInner).
                return share.writeInner ? 2 * n : 0;
            default:
                // filter, project, rename, limit: pipelined, no I/O (§M4.4).
                return 0;
            }
        }

        struct CostWalk
        {
            const std::vector<exec::FrameShare>& shares;
            std::vector<NodeEstimate>& estimates;
            // Pre-order index of the next node the walk visits.
            std::size_t next = 0;
        };

        // Fills every node's cost, in pre-order (the order of the shares and
        // the estimates). Kept this small because it recurses once per tree
        // level (up to kMaxNestingDepth); the work is in nodeCost.
        void costWalk(const PlanNode& node, CostWalk& walk)
        {
            std::size_t self = walk.next++;
            std::size_t right = 0;
            for (std::size_t i = 0; i < node.children.size(); ++i)
            {
                if (i == 1)
                {
                    right = walk.next;
                }
                costWalk(node.children[i], walk);
            }
            walk.estimates[self].cost = nodeCost(node, walk.shares[self], walk.estimates, self, right);
        }

        // Gives every estimate of `physical` its node's cost at B = frames.
        // divideFrames throws Error(Schema) when the plan needs more than B.
        void addCosts(PhysicalResult& physical, std::int64_t frames)
        {
            exec::FramePlan framePlan = exec::divideFrames(physical.plan, static_cast<std::size_t>(frames));
            CostWalk walk{framePlan.shares, physical.estimates};
            costWalk(physical.plan, walk);
        }

        // -------------------------------------------------------------------
        // Join blocks and join ordering.

        // What the join-ordering walk needs everywhere.
        struct Planner
        {
            const StatsProvider& stats;
            const TableSchemaLookup& tableSchema;
            std::int64_t frames;
            // Rule 3 on: a condition of a select inside a block that uses
            // two or more relations may become a join condition.
            bool selectsBecomeJoins;
            std::vector<DpTable>& dpTables;
        };

        bool isJoinOrCross(PlanOp op)
        {
            return op == PlanOp::Join || op == PlanOp::Cross;
        }

        // True when `node` is part of a join block: a join or cross, or a
        // select or project with a join or cross right below it (through
        // other selects and projects). Selects in a block give the DP their
        // conditions; projects in a block only drop columns, and rule 6
        // puts them back after the joins are reordered.
        bool inJoinBlock(const PlanNode& node)
        {
            const PlanNode* below = &node;
            while (below->op == PlanOp::Select || below->op == PlanOp::Project)
            {
                below = &below->children[0];
            }
            return isJoinOrCross(below->op);
        }

        // A join block taken apart.
        struct Block
        {
            // The relations: the subtrees right below the block, in written
            // order (left to right). They point into the block's tree.
            std::vector<PlanNode*> leaves;
            // The conjuncts of the joins' conditions.
            std::vector<Condition> joinParts;
            // The conjuncts of the selects inside the block.
            std::vector<Condition> selectParts;
        };

        // Takes apart the block whose top node is `top`. Uses its own stack
        // instead of recursion: a block may hold about 1,000 stacked selects
        // (rule 7 off).
        Block collectBlock(PlanNode& top)
        {
            Block block;
            std::vector<PlanNode*> stack{&top};
            while (!stack.empty())
            {
                PlanNode* node = stack.back();
                stack.pop_back();
                if (!inJoinBlock(*node))
                {
                    block.leaves.push_back(node);
                    continue;
                }
                if (node->op == PlanOp::Join)
                {
                    for (Condition& part : splitConjuncts(*node->condition))
                    {
                        block.joinParts.push_back(std::move(part));
                    }
                }
                else if (node->op == PlanOp::Select)
                {
                    for (Condition& part : splitConjuncts(*node->condition))
                    {
                        block.selectParts.push_back(std::move(part));
                    }
                }
                // Right child first, so the left one comes off the stack next
                // and the leaves come out left to right.
                for (std::size_t i = node->children.size(); i-- > 0;)
                {
                    stack.push_back(&node->children[i]);
                }
            }
            return block;
        }

        // The name of a relation in the DP table: the alias of the first
        // rename met going down, else the scanned table (`e`, `A`). Every
        // leaf ends in a scan.
        std::string relationName(const PlanNode& leaf)
        {
            const PlanNode* node = &leaf;
            while (node->op != PlanOp::Rename && node->op != PlanOp::Scan)
            {
                node = &node->children[0];
            }
            return node->op == PlanOp::Rename ? node->alias : node->table;
        }

        // True when `schema` has a column `ref` names (qualified: exact
        // match; bare: by name).
        bool hasColumn(const Schema& schema, const ColumnRef& ref)
        {
            for (const Column& column : schema.columns())
            {
                if (column.name == ref.name && (!ref.qualifier || *ref.qualifier == column.qualifier))
                {
                    return true;
                }
            }
            return false;
        }

        // The relations whose columns `condition` uses, as a bitmask.
        std::uint32_t relationsUsed(const Condition& condition, const std::vector<Schema>& schemas)
        {
            std::uint32_t mask = 0;
            for (const ColumnRef& ref : columnsUsed(condition))
            {
                for (std::size_t i = 0; i < schemas.size(); ++i)
                {
                    if (hasColumn(schemas[i], ref))
                    {
                        mask |= std::uint32_t{1} << i;
                    }
                }
            }
            return mask;
        }

        // The estimated size of the group `mask` once joined: its relations
        // joined in index order, each predicate on the first join where all
        // its relations are in, then estimatePlan on that tree. So the DP
        // uses the same join estimate (§M4.2) as the final plan.
        NodeEstimate groupSize(std::uint32_t mask, const std::vector<PlanNode*>& leaves, const JoinGraph& graph,
                               const StatsProvider& stats)
        {
            PlanNode tree;
            std::uint32_t inTree = 0;
            for (std::size_t i = 0; i < leaves.size(); ++i)
            {
                std::uint32_t bit = std::uint32_t{1} << i;
                if ((mask & bit) == 0)
                {
                    continue;
                }
                if (inTree == 0)
                {
                    tree = *leaves[i];
                    inTree = bit;
                    continue;
                }
                inTree |= bit;
                std::vector<Condition> conditions;
                for (const JoinPredicate& predicate : graph.predicates)
                {
                    // Usable here: it needs relation i, and all its
                    // relations are now in the tree.
                    if ((predicate.relations & bit) != 0 && (predicate.relations & ~inTree) == 0)
                    {
                        conditions.push_back(predicate.condition);
                    }
                }
                if (conditions.empty())
                {
                    tree = PlanNode::cross(std::move(tree), *leaves[i]);
                }
                else
                {
                    tree = PlanNode::join(conjoin(std::move(conditions)), std::move(tree), *leaves[i]);
                }
            }
            return estimatePlan(tree, stats)[0];
        }

        // The qualified names of a schema's columns, in order.
        std::vector<std::string> columnNames(const Schema& schema)
        {
            std::vector<std::string> names;
            for (const Column& column : schema.columns())
            {
                names.push_back(column.qualifiedName());
            }
            return names;
        }

        // Builds the block again from the DP's answer: start with relation
        // `first`, join one relation per step (a cross when the step has no
        // condition). Conditions on fewer than two relations (`leftovers`)
        // go in one select on top. If the columns are no longer those of the
        // block as written (`original`, same names in the same order), a
        // project puts them back: joining in another order changes the
        // column order, and `SELECT *` must keep the written one.
        PlanNode rebuild(const Block& block, const DpTable& table, std::vector<Condition> leftovers,
                         const Schema& original, const TableSchemaLookup& tableSchema)
        {
            PlanNode tree = std::move(*block.leaves[table.first]);
            for (const JoinStep& step : table.steps)
            {
                PlanNode inner = std::move(*block.leaves[step.inner]);
                if (step.conditions.empty())
                {
                    tree = PlanNode::cross(std::move(tree), std::move(inner));
                }
                else
                {
                    tree = PlanNode::join(conjoin(step.conditions), std::move(tree), std::move(inner));
                }
            }
            if (!leftovers.empty())
            {
                tree = PlanNode::select(conjoin(std::move(leftovers)), std::move(tree));
            }
            if (columnNames(outputSchema(tree, tableSchema)) != columnNames(original))
            {
                std::vector<ColumnRef> columns;
                for (const Column& column : original.columns())
                {
                    std::optional<std::string> qualifier;
                    if (!column.qualifier.empty())
                    {
                        qualifier = column.qualifier;
                    }
                    columns.push_back(ColumnRef{qualifier, column.name});
                }
                tree = PlanNode::project(std::move(columns), std::move(tree));
            }
            return tree;
        }

        void orderJoins(PlanNode& node, Planner& planner);

        // Plans the join block whose top node is `top` and replaces it with
        // the reordered block. The blocks inside its relations (derived
        // tables) are planned first, so their DP tables come first.
        void planBlock(PlanNode& top, Planner& planner)
        {
            Schema original = outputSchema(top, planner.tableSchema);
            Block block = collectBlock(top);
            for (PlanNode* leaf : block.leaves)
            {
                orderJoins(*leaf, planner);
            }

            JoinGraph graph;
            for (const PlanNode* leaf : block.leaves)
            {
                graph.relations.push_back(JoinRelation{relationName(*leaf), 0, 0, 0});
            }
            if (block.leaves.size() > kMaxDpRelations)
            {
                // Written order kept; the table has the names, no entries.
                DpTable table;
                for (const JoinRelation& relation : graph.relations)
                {
                    table.names.push_back(relation.name);
                }
                planner.dpTables.push_back(std::move(table));
                return;
            }

            // Each relation alone: its access path cost (§M4.5 best[{r}]) is
            // the sum of its nodes' costs at the full B, and its size the
            // estimate of its top node.
            std::vector<Schema> schemas;
            for (std::size_t i = 0; i < block.leaves.size(); ++i)
            {
                schemas.push_back(outputSchema(*block.leaves[i], planner.tableSchema));
                PhysicalResult alone = toPhysical(*block.leaves[i], planner.stats, planner.frames);
                addCosts(alone, planner.frames);
                for (const NodeEstimate& estimate : alone.estimates)
                {
                    graph.relations[i].cost += estimate.cost;
                }
                graph.relations[i].pages = alone.estimates[0].pages;
                graph.relations[i].rows = alone.estimates[0].rows;
            }

            // The predicates: every conjunct that uses two or more
            // relations. A select's conjuncts only with rule 3 on, else they
            // stay above the block like the rest (`leftovers`).
            std::vector<Condition> leftovers;
            std::vector<Condition> parts = std::move(block.joinParts);
            if (planner.selectsBecomeJoins)
            {
                parts.insert(parts.end(), block.selectParts.begin(), block.selectParts.end());
            }
            else
            {
                leftovers = std::move(block.selectParts);
            }
            for (Condition& part : parts)
            {
                std::uint32_t relations = relationsUsed(part, schemas);
                if (std::popcount(relations) < 2)
                {
                    leftovers.push_back(std::move(part));
                    continue;
                }
                // Sort-merge and hash can run it: a column = column equality
                // between exactly two relations, same type on both sides.
                bool equality = false;
                if (std::popcount(relations) == 2)
                {
                    auto first = static_cast<std::size_t>(std::countr_zero(relations));
                    auto second = static_cast<std::size_t>(std::countr_zero(relations & (relations - 1)));
                    equality = mergeOrHashValid(part, schemas[first], schemas[second]);
                }
                graph.predicates.push_back(JoinPredicate{std::move(part), relations, equality});
            }

            // The size of every group of two or more relations.
            std::vector<NodeEstimate> sizes(std::size_t{1} << block.leaves.size());
            for (std::uint32_t mask = 0; mask < sizes.size(); ++mask)
            {
                if (std::popcount(mask) >= 2)
                {
                    sizes[mask] = groupSize(mask, block.leaves, graph, planner.stats);
                }
            }

            DpTable table = enumerateJoins(graph, sizes, planner.frames);
            PlanNode rebuilt = rebuild(block, table, std::move(leftovers), original, planner.tableSchema);
            top = std::move(rebuilt);
            planner.dpTables.push_back(std::move(table));
        }

        // Finds every join block of the tree and plans it, left to right.
        // Kept this small because it recurses once per tree level (up to
        // kMaxNestingDepth); the work is in planBlock.
        void orderJoins(PlanNode& node, Planner& planner)
        {
            if (inJoinBlock(node))
            {
                planBlock(node, planner);
                return;
            }
            for (PlanNode& child : node.children)
            {
                orderJoins(child, planner);
            }
        }

        void turnOff(RuleSet& rules, RuleId rule)
        {
            rules[static_cast<std::size_t>(rule) - 1] = false;
        }
    } // namespace

    OptimizedPlan optimize(const PlanNode& logical, const StatsProvider& stats, const OptimizerOptions& options)
    {
        TableSchemaLookup tableSchema = [&stats](const std::string& name) -> std::optional<Schema>
        {
            std::optional<storage::TableInfo> info = stats(name);
            if (!info)
            {
                return std::nullopt;
            }
            return info->schema;
        };

        // Step 1: an unknown table or column, or a type error, fails here
        // with common's message, before any work.
        outputSchema(logical, tableSchema);

        // Step 2: rewrite. The unoptimized plan (§M4.7 #2) turns off the
        // pushdown rules only: 2, 4, 5 (selects) and 6 (projections).
        RuleSet rules = options.rules;
        if (options.unoptimized)
        {
            rules = kAllRules;
            turnOff(rules, RuleId::PushSelectBelowJoin);
            turnOff(rules, RuleId::PushSelectBelowRenameUnion);
            turnOff(rules, RuleId::PushSelectBelowGroupBy);
            turnOff(rules, RuleId::PushProjections);
        }
        OptimizedPlan result;
        result.logicalBefore = logical;
        result.logicalAfter = rewritePlan(logical, rules, tableSchema);

        // Steps 3 to 5: order the joins, unless unoptimized (written order,
        // one fixed algorithm).
        PlanNode ordered = result.logicalAfter;
        std::optional<JoinAlgorithm> fixedAlgorithm;
        if (options.unoptimized)
        {
            fixedAlgorithm = options.fixedAlgorithm;
        }
        else
        {
            Planner planner{
                stats, tableSchema, options.frames, enabled(rules, RuleId::CrossToJoin),
                result.dpTables
            };
            orderJoins(ordered, planner);
            // Rule 6 again for the new join positions (and rule 7, which
            // removes a column-order project that a parent project makes
            // redundant). Rules 1 to 5 are off: their work is done.
            RuleSet projections{};
            projections[static_cast<std::size_t>(RuleId::PushProjections) - 1] =
                enabled(rules, RuleId::PushProjections);
            projections[static_cast<std::size_t>(RuleId::RemoveRedundant) - 1] =
                enabled(rules, RuleId::RemoveRedundant);
            ordered = rewritePlan(ordered, projections, tableSchema);
        }

        // Step 6: the physical plan, its estimates and every node's cost.
        PhysicalResult physical = toPhysical(ordered, stats, options.frames, fixedAlgorithm);
        addCosts(physical, options.frames);
        result.physical = std::move(physical.plan);
        result.estimates = std::move(physical.estimates);
        return result;
    }
} // namespace db::opt
