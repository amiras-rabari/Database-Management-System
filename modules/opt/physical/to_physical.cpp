#include "db/opt/physical.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/opt/cost.h"
#include "db/opt/estimate.h"
#include "db/storage/table_info.h"

namespace db::opt
{
    namespace
    {
        using TableSchemaLookup = std::function<std::optional<Schema>(const std::string&)>;

        // One aggregate output column as the nodes above see it. Right above
        // its group_by the column is bare (`n`); above a rename it carries
        // the rename's alias (`t.n`, sql example 5.6).
        struct AggregateName
        {
            // "" right above the group_by, else the alias of the rename above.
            std::string qualifier;
            // The logical name: the SQL alias, e.g. `n`.
            std::string alias;
            // The physical name from aggregateOutputName, e.g. `count_star`.
            std::string generated;
        };

        // The aggregate columns in a node's output.
        using AggregateNames = std::vector<AggregateName>;

        // What the conversion walk carries from node to node.
        struct Walk
        {
            // Estimates of the (split) logical tree, in pre-order.
            const std::vector<NodeEstimate>& estimates;
            const TableSchemaLookup& tableSchema;
            std::int64_t frames;
            std::optional<JoinAlgorithm> fixedAlgorithm;
            // Pre-order index of the next node the walk visits.
            std::size_t next = 0;
        };

        // Splits one join condition that mixes equalities usable by
        // sort-merge/hash with other conjuncts: `join[R.b = S.b and R.a <
        // S.c]` becomes `select[R.a < S.c](join[R.b = S.b])`. Then the join
        // can still use hash or sort-merge, and exec needs no extra condition
        // on them. A condition that is all usable equalities, or has none,
        // stays as it is.
        void splitJoin(PlanNode& node, const TableSchemaLookup& tableSchema)
        {
            Schema left = outputSchema(node.children[0], tableSchema);
            Schema right = outputSchema(node.children[1], tableSchema);
            std::vector<Condition> equalities;
            std::vector<Condition> rest;
            for (Condition& part : splitConjuncts(*node.condition))
            {
                if (mergeOrHashValid(part, left, right))
                {
                    equalities.push_back(std::move(part));
                }
                else
                {
                    rest.push_back(std::move(part));
                }
            }
            if (equalities.empty() || rest.empty())
            {
                return;
            }
            node.condition = conjoin(std::move(equalities));
            PlanNode join = std::move(node);
            node = PlanNode::select(conjoin(std::move(rest)), std::move(join));
        }

        // splitJoin on every join of the tree, in place. Kept this small
        // because it recurses once per tree level (up to kMaxNestingDepth):
        // the real work happens in splitJoin, whose locals exist only while
        // it runs.
        void splitMixedJoins(PlanNode& node, const TableSchemaLookup& tableSchema)
        {
            for (PlanNode& child : node.children)
            {
                splitMixedJoins(child, tableSchema);
            }
            if (node.op == PlanOp::Join)
            {
                splitJoin(node, tableSchema);
            }
        }

        // The physical name of `ref`: the generated name if it refers to an
        // aggregate in `names`, else `ref` unchanged. A bare `n` matches any
        // aggregate named n (outputSchema already made sure only one can
        // exist); `t.n` only the one under rename t.
        ColumnRef physicalName(const ColumnRef& ref, const AggregateNames& names)
        {
            for (const AggregateName& name : names)
            {
                if (ref.name == name.alias && (!ref.qualifier || *ref.qualifier == name.qualifier))
                {
                    return ColumnRef{ref.qualifier, name.generated};
                }
            }
            return ref;
        }

        // The join algorithm for a join of inputs of m and n pages.
        // `valid` = sort-merge and hash may be used (mergeOrHashValid).
        JoinAlgorithm chooseJoin(bool valid, std::int64_t m, std::int64_t n, const Walk& walk)
        {
            // Unoptimized plan: the fixed algorithm wherever exec accepts it.
            if (walk.fixedAlgorithm)
            {
                return valid ? *walk.fixedAlgorithm : JoinAlgorithm::NestedLoop;
            }
            // Cheapest at the full B. Only the join's own formula is compared:
            // what its inputs cost is the same whichever algorithm wins.
            // "<" (not "<=") keeps the earlier algorithm on a tie.
            JoinAlgorithm best = JoinAlgorithm::NestedLoop;
            std::int64_t bestCost = costNestedLoopJoin(m, n, walk.frames);
            if (valid)
            {
                std::int64_t sortMerge = costSortMergeJoin(m, n, walk.frames);
                if (sortMerge < bestCost)
                {
                    best = JoinAlgorithm::SortMerge;
                    bestCost = sortMerge;
                }
                if (costHashJoin(m, n) < bestCost)
                {
                    best = JoinAlgorithm::Hash;
                }
            }
            return best;
        }

        PlanOp joinOp(JoinAlgorithm algorithm)
        {
            switch (algorithm)
            {
            case JoinAlgorithm::SortMerge:
                return PlanOp::SortMergeJoin;
            case JoinAlgorithm::Hash:
                return PlanOp::HashJoin;
            case JoinAlgorithm::NestedLoop:
                break;
            }
            return PlanOp::NestedLoopJoin;
        }

        // Sort or hash for a distinct/group_by reading m pages whose groups
        // fill groupPages pages. Sort costs sort(m). Hash gets a table of
        // B - 1 frames: exec gives it everything its input does not hold
        // while streaming, and a scan holds 1 page (exec's phase table).
        // Hash only when strictly cheaper, so a tie gives sort.
        ExecMode chooseMode(std::int64_t m, std::int64_t groupPages, std::int64_t frames)
        {
            std::int64_t sortCost = costSort(m, frames);
            std::int64_t hashCost = costHashGroup(m, groupPages, frames - 1);
            return hashCost < sortCost ? ExecMode::Hash : ExecMode::Sort;
        }

        // Fills the physical node `out` from the logical node `logical`,
        // whose children are already converted (`out.children`). `self` and
        // `rightIndex` are the pre-order indexes of the node and of its right
        // child, used to read their estimates. `names` holds the aggregate
        // columns of the left (or only) child's output on entry, and of this
        // node's output on return; `rightNames` those of the right child.
        void convertNode(const PlanNode& logical, PlanNode& out, std::size_t self, std::size_t rightIndex,
                         const AggregateNames& rightNames, const Walk& walk, AggregateNames& names)
        {
            out.op = logical.op;
            out.table = logical.table;
            out.alias = logical.alias;
            out.limitCount = logical.limitCount;
            // A join's output has both sides' columns; a set operation's has
            // the left one's (common's checkUnionCompatible).
            if (logical.op == PlanOp::Join || logical.op == PlanOp::Cross)
            {
                names.insert(names.end(), rightNames.begin(), rightNames.end());
            }

            // Every column this node names: aliases become generated names.
            auto rename = [&names](const ColumnRef& ref) { return physicalName(ref, names); };
            if (logical.condition)
            {
                out.condition = mapColumns(*logical.condition, rename);
            }
            for (const ColumnRef& column : logical.columns)
            {
                out.columns.push_back(rename(column));
            }
            for (const SortKey& key : logical.sortKeys)
            {
                out.sortKeys.push_back(SortKey{rename(key.column), key.ascending});
            }
            for (const Aggregate& aggregate : logical.aggregates)
            {
                std::optional<ColumnRef> column;
                if (aggregate.column)
                {
                    column = rename(*aggregate.column);
                }
                // A physical aggregate has no alias: its name is generated.
                out.aggregates.push_back(Aggregate{aggregate.function, column, ""});
            }

            // In pre-order the left (or only) child comes right after the node.
            std::size_t leftIndex = self + 1;
            switch (logical.op)
            {
            case PlanOp::Scan:
                out.op = PlanOp::SeqScan;
                break;
            case PlanOp::Select:
                out.op = PlanOp::Filter;
                break;
            case PlanOp::Rename:
                // Above a rename the aggregates carry its alias: t.n.
                for (AggregateName& name : names)
                {
                    name.qualifier = logical.alias;
                }
                break;
            case PlanOp::Join:
            case PlanOp::Cross: {
                bool valid = false;
                if (logical.op == PlanOp::Join)
                {
                    // Checked on the logical names: the schemas below are
                    // logical too.
                    valid = mergeOrHashValid(*logical.condition,
                                             outputSchema(logical.children[0], walk.tableSchema),
                                             outputSchema(logical.children[1], walk.tableSchema));
                }
                else
                {
                    // Exec has no join without a condition: `1 = 1` pairs every
                    // left tuple with every right one. It has no keys, so only
                    // the nested loop is valid.
                    out.condition = Condition::compare(Value::ofInt(1), CompareOp::Equal, Value::ofInt(1));
                }
                std::int64_t m = walk.estimates[leftIndex].pages;
                std::int64_t n = walk.estimates[rightIndex].pages;
                out.op = joinOp(chooseJoin(valid, m, n, walk));
                break;
            }
            case PlanOp::Distinct:
                out.mode = chooseMode(walk.estimates[leftIndex].pages, walk.estimates[self].pages, walk.frames);
                break;
            case PlanOp::GroupBy:
                out.mode = chooseMode(walk.estimates[leftIndex].pages, walk.estimates[self].pages, walk.frames);
                for (std::size_t i = 0; i < logical.aggregates.size(); ++i)
                {
                    names.push_back(AggregateName{"", logical.aggregates[i].alias,
                                                  aggregateOutputName(out.aggregates[i])});
                }
                break;
            default:
                // project, sort, limit and the set operations keep their op.
                break;
            }
        }

        // Converts `logical` and its subtree into `out`, in pre-order (the
        // order of walk.estimates, so walk.next is each node's estimate
        // index). Kept this small because it recurses once per tree level:
        // children are built in place and the work is in convertNode.
        void convert(const PlanNode& logical, PlanNode& out, Walk& walk, AggregateNames& names)
        {
            std::size_t self = walk.next++;
            std::size_t rightIndex = 0;
            AggregateNames rightNames;
            out.children.resize(logical.children.size());
            for (std::size_t i = 0; i < logical.children.size(); ++i)
            {
                if (i == 1)
                {
                    rightIndex = walk.next;
                }
                // The left (or only) child's aggregate names go straight
                // into `names`; convertNode adds the right ones for a join.
                convert(logical.children[i], out.children[i], walk, i == 0 ? names : rightNames);
            }
            convertNode(logical, out, self, rightIndex, rightNames, walk, names);
        }
    } // namespace

    bool mergeOrHashValid(const Condition& condition, const Schema& left, const Schema& right)
    {
        std::optional<std::vector<std::pair<std::size_t, std::size_t>>> keys = equiJoinKeys(condition, left, right);
        if (!keys)
        {
            return false;
        }
        for (const std::pair<std::size_t, std::size_t>& key : *keys)
        {
            // INT against REAL (an AVG from a derived table) compares as
            // numbers in a nested loop, but exec's sort-merge and hash joins
            // need the same type.
            if (left.column(key.first).type != right.column(key.second).type)
            {
                return false;
            }
        }
        return true;
    }

    PhysicalResult toPhysical(const PlanNode& logical, const StatsProvider& stats, std::int64_t frames,
                              std::optional<JoinAlgorithm> fixedAlgorithm)
    {
        TableSchemaLookup tableSchema = [&stats](const std::string& name) -> std::optional<Schema> {
            std::optional<storage::TableInfo> info = stats(name);
            if (!info)
            {
                return std::nullopt;
            }
            return info->schema;
        };
        // Split first and estimate the split tree, so the logical and the
        // physical tree have the same nodes in the same pre-order and the
        // estimates line up one to one.
        PlanNode split = logical;
        splitMixedJoins(split, tableSchema);
        PhysicalResult result;
        result.estimates = estimatePlan(split, stats);
        Walk walk{result.estimates, tableSchema, frames, fixedAlgorithm};
        AggregateNames names;
        convert(split, result.plan, walk, names);
        return result;
    }
} // namespace db::opt
