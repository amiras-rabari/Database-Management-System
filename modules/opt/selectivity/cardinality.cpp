#include "db/opt/estimate.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/opt/defaults.h"
#include "db/opt/selectivity.h"
#include "db/opt/statistics.h"
#include "db/storage/page.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    namespace
    {
        /// What a node hands up to its parent.
        struct NodeOutput
        {
            /// The node's output columns (common's per-node schema rules).
            Schema schema;
            /// One entry per output column, same order as `schema`.
            std::vector<ColumnFacts> facts;
            std::int64_t rows = 1;
            double width = 0.0;
            /// The page count when it is known exactly (a scan reads the
            /// table's real data pages); 0 = compute it from rows and width.
            std::int64_t knownPages = 0;
        };

        /// State shared by the whole walk.
        struct Walk
        {
            const StatsProvider& stats;
            /// The TableInfo of every scanned table. ColumnFacts point into
            /// them, so they must live until the walk ends. A deque never
            /// moves its elements when a new one is added, so the pointers
            /// stay valid.
            std::deque<storage::TableInfo> tables;
            /// The answer, in pre-order.
            std::vector<NodeEstimate> estimates;
        };

        NodeOutput estimateNode(const PlanNode& node, Walk& walk);

        /// After a node keeps `rows` rows, no column can have more distinct
        /// values than that.
        void capNdv(std::vector<ColumnFacts>& facts, std::int64_t rows)
        {
            for (ColumnFacts& f : facts)
            {
                f.ndv = std::min(f.ndv, rows);
            }
        }

        double sumWidths(const std::vector<ColumnFacts>& facts)
        {
            double width = 0.0;
            for (const ColumnFacts& f : facts)
            {
                width += f.width;
            }
            return width;
        }

        /// How many distinct combinations the columns at `positions` can
        /// have: the product of their ndv, but never more than `rows`. A
        /// column with unknown ndv could be all different, so the answer is
        /// then `rows`. No positions (group_by without keys) gives 1.
        std::int64_t distinctRows(std::int64_t rows, const std::vector<ColumnFacts>& facts,
                                  const std::vector<std::size_t>& positions)
        {
            double product = 1.0;
            for (std::size_t p : positions)
            {
                if (facts[p].ndv == 0)
                {
                    return rows;
                }
                product *= static_cast<double>(facts[p].ndv);
            }
            return std::min(rows, ceilRows(product));
        }

        /// The schema of every table, for common's outputSchema.
        std::optional<Schema> tableSchemaOf(const StatsProvider& stats, const std::string& name)
        {
            std::optional<storage::TableInfo> info = stats(name);
            if (!info)
            {
                return std::nullopt;
            }
            return info->schema;
        }

        NodeOutput estimateScan(const PlanNode& node, Walk& walk)
        {
            std::optional<storage::TableInfo> found = walk.stats(node.table);
            if (!found)
            {
                throw Error(ErrorCategory::Name, "Unknown table '" + node.table + "'.");
            }
            const storage::TableInfo& table = walk.tables.emplace_back(std::move(*found));

            NodeOutput out;
            out.schema = table.schema;
            // Rows and pages come from the catalog, exact even without
            // ANALYZE (statistics.h). A scan reads every data page.
            out.rows = ceilRows(static_cast<double>(table.numTuples));
            out.knownPages = std::max<std::int64_t>(1, table.numPages);
            out.width = tableWidth(table);

            std::vector<double> widths = columnWidths(table);
            for (std::size_t i = 0; i < table.schema.size(); ++i)
            {
                const Column& column = table.schema.column(i);
                const storage::ColumnStats* stats = columnStats(table, ColumnRef{column.qualifier, column.name});
                std::int64_t ndv = stats ? static_cast<std::int64_t>(stats->ndv) : 0;
                out.facts.push_back(ColumnFacts{stats, ndv, widths[i]});
            }
            capNdv(out.facts, out.rows);
            return out;
        }

        NodeOutput estimateJoinOrCross(const PlanNode& node, Walk& walk)
        {
            NodeOutput left = estimateNode(node.children[0], walk);
            NodeOutput right = estimateNode(node.children[1], walk);

            // The output columns are the left ones, then the right ones (C3).
            NodeOutput out;
            out.schema = concatSchemas(left.schema, right.schema);
            out.facts = left.facts;
            out.facts.insert(out.facts.end(), right.facts.begin(), right.facts.end());
            out.width = left.width + right.width;

            // A cross keeps every pair. A join keeps a fraction of them: one
            // factor per conjunct of its condition, multiplied (independence,
            // as for AND). For one equality this is §M4.2's
            // n(R) x n(S) / max(ndv(R.a), ndv(S.b)).
            double rows = static_cast<double>(left.rows) * static_cast<double>(right.rows);
            if (node.op == PlanOp::Join)
            {
                for (const Condition& conjunct : splitConjuncts(*node.condition))
                {
                    rows *= joinConjunctSelectivity(conjunct, left.schema, right.schema, out.facts);
                }
            }
            out.rows = ceilRows(rows);
            capNdv(out.facts, out.rows);
            return out;
        }

        NodeOutput estimateGroupBy(const PlanNode& node, Walk& walk)
        {
            NodeOutput in = estimateNode(node.children[0], walk);

            std::vector<std::size_t> keys;
            for (const ColumnRef& key : node.columns)
            {
                keys.push_back(in.schema.resolve(key));
            }

            // One row per group: as many as there are distinct key
            // combinations (spec silent; settled in the chunk guide).
            NodeOutput out;
            out.rows = distinctRows(in.rows, in.facts, keys);
            out.schema = outputSchema(node, [&walk](const std::string& name)
            {
                return tableSchemaOf(walk.stats, name);
            });

            // The keys keep what is known about them; an aggregate is a new
            // value with no statistics. Widths: count and sum are INT
            // (4 bytes), avg is REAL (8 bytes), min and max as wide as their
            // column.
            for (std::size_t key : keys)
            {
                out.facts.push_back(in.facts[key]);
            }
            for (const Aggregate& aggregate : node.aggregates)
            {
                double width = 4.0;
                if (aggregate.function == AggregateFunction::Avg)
                {
                    width = 8.0;
                }
                else if (aggregate.function == AggregateFunction::Min || aggregate.function == AggregateFunction::Max)
                {
                    width = in.facts[in.schema.resolve(*aggregate.column)].width;
                }
                out.facts.push_back(ColumnFacts{nullptr, 0, width});
            }
            out.width = sumWidths(out.facts);
            capNdv(out.facts, out.rows);
            return out;
        }

        /// union, union_all, intersect, except: the output has the left
        /// schema (checkUnionCompatible).
        NodeOutput estimateSetOperation(const PlanNode& node, Walk& walk)
        {
            NodeOutput left = estimateNode(node.children[0], walk);
            NodeOutput right = estimateNode(node.children[1], walk);
            NodeOutput out = left;

            if (node.op == PlanOp::Union || node.op == PlanOp::UnionAll)
            {
                // Every row of both sides. For union this is an upper bound:
                // over-estimating what dedupe removes is the safe side.
                out.rows = left.rows + right.rows;
                for (std::size_t i = 0; i < out.facts.size(); ++i)
                {
                    bool bothKnown = left.facts[i].ndv > 0 && right.facts[i].ndv > 0;
                    out.facts[i].ndv = bothKnown ? left.facts[i].ndv + right.facts[i].ndv : 0;
                }
            }
            else if (node.op == PlanOp::Intersect)
            {
                // Only rows found on both sides: at most the smaller side.
                out.rows = std::min(left.rows, right.rows);
            }
            // except: at most every left row (out.rows is already left.rows).

            capNdv(out.facts, out.rows);
            return out;
        }

        /// The rules for one node; `node`'s children are estimated inside.
        NodeOutput computeNode(const PlanNode& node, Walk& walk)
        {
            switch (node.op)
            {
            case PlanOp::Scan:
                return estimateScan(node, walk);

            case PlanOp::Select:
                {
                    NodeOutput out = estimateNode(node.children[0], walk);
                    // The selectivity code asks for a column's statistics by
                    // name; the name is found in this node's schema, and its
                    // position gives the facts.
                    const Schema& schema = out.schema;
                    const std::vector<ColumnFacts>& facts = out.facts;
                    ColumnStatsLookup lookup = [&schema, &facts](const ColumnRef& ref)
                    {
                        return facts[schema.resolve(ref)].stats;
                    };
                    double selectivity = estimateSelectivity(*node.condition, lookup);
                    out.rows = ceilRows(static_cast<double>(out.rows) * selectivity);
                    out.knownPages = 0;
                    capNdv(out.facts, out.rows);
                    return out;
                }

            case PlanOp::Project:
                {
                    NodeOutput in = estimateNode(node.children[0], walk);
                    NodeOutput out;
                    std::vector<std::size_t> kept;
                    out.schema = projectSchema(in.schema, node.columns, &kept);
                    for (std::size_t i : kept)
                    {
                        out.facts.push_back(in.facts[i]);
                    }
                    out.rows = in.rows;
                    // Only the kept columns' bytes remain (§M4.2: how a
                    // projection reduces later costs).
                    out.width = sumWidths(out.facts);
                    return out;
                }

            case PlanOp::Rename:
                {
                    // Same rows, same columns in the same positions, new names.
                    NodeOutput out = estimateNode(node.children[0], walk);
                    out.schema = requalify(out.schema, node.alias);
                    return out;
                }

            case PlanOp::Join:
            case PlanOp::Cross:
                return estimateJoinOrCross(node, walk);

            case PlanOp::GroupBy:
                return estimateGroupBy(node, walk);

            case PlanOp::Distinct:
                {
                    NodeOutput out = estimateNode(node.children[0], walk);
                    std::vector<std::size_t> all;
                    for (std::size_t i = 0; i < out.facts.size(); ++i)
                    {
                        all.push_back(i);
                    }
                    out.rows = distinctRows(out.rows, out.facts, all);
                    out.knownPages = 0;
                    return out;
                }

            case PlanOp::Sort:
                // Same rows in another order.
                return estimateNode(node.children[0], walk);

            case PlanOp::Limit:
                {
                    NodeOutput out = estimateNode(node.children[0], walk);
                    out.rows = ceilRows(static_cast<double>(std::min<std::int64_t>(node.limitCount, out.rows)));
                    out.knownPages = 0;
                    capNdv(out.facts, out.rows);
                    return out;
                }

            case PlanOp::Union:
            case PlanOp::UnionAll:
            case PlanOp::Intersect:
            case PlanOp::Except:
                return estimateSetOperation(node, walk);

            default:
                // SeqScan, Filter and the three physical joins: this
                // estimator reads logical plans only.
                throw std::logic_error("estimatePlan: physical operator in a logical plan");
            }
        }

        /// Reserves this node's pre-order slot BEFORE its children take
        /// theirs, then fills it once the children are done.
        NodeOutput estimateNode(const PlanNode& node, Walk& walk)
        {
            std::size_t slot = walk.estimates.size();
            // reserve the parents slot for now  and fill it later
            walk.estimates.emplace_back();

            // children fill their slots after parent
            NodeOutput out = computeNode(node, walk);

            std::int64_t pages = out.knownPages > 0 ? out.knownPages : ceilPages(out.rows, out.width);
            walk.estimates[slot] = NodeEstimate{out.rows, pages, out.width, 0};
            return out;
        }
    } // namespace

    std::int64_t ceilRows(double x)
    {
        // Take a tiny amount off before rounding up, so a value that is a
        // whole number on paper but a hair above it in binary (0.1 x 3 x 10
        // = 3.0000000000000004) stays that whole number. The tolerance grows
        // with x, because doubles lose precision as numbers grow.
        double tolerance = 1e-9 * std::max(1.0, std::fabs(x));
        double up = std::ceil(x - tolerance);
        if (up < 1.0)
        {
            return 1;
        }
        return static_cast<std::int64_t>(up);
    }

    std::int64_t ceilPages(std::int64_t rows, double width)
    {
        return ceilRows(static_cast<double>(rows) * width / static_cast<double>(storage::kPageSize));
    }

    double joinConjunctSelectivity(const Condition& conjunct, const Schema& left, const Schema& right,
                                   const std::vector<ColumnFacts>& facts)
    {
        // `L.a = R.b` with one column on each side: equiJoinKeys gives the
        // pair (position of a in left, position of b in right).
        std::optional<std::vector<std::pair<std::size_t, std::size_t>>> keys = equiJoinKeys(conjunct, left, right);
        if (keys)
        {
            double selectivity = 1.0;
            for (const auto& [leftIndex, rightIndex] : *keys)
            {
                std::int64_t leftNdv = facts[leftIndex].ndv;
                std::int64_t rightNdv = facts[left.size() + rightIndex].ndv;
                // max(ndv(L.a), ndv(R.b)); an unknown ndv is 0, so max uses
                // the known one alone. Neither known: the default guess.
                std::int64_t most = std::max(leftNdv, rightNdv);
                selectivity *= most > 0 ? 1.0 / static_cast<double>(most) : kDefaultEqualitySelectivity;
            }
            return selectivity;
        }

        // Anything else is an ordinary condition over the join's columns.
        // Column against column gives 0.1 there, which is §M4.2's "any other
        // join predicate".
        Schema both = concatSchemas(left, right);
        ColumnStatsLookup lookup = [&both, &facts](const ColumnRef& ref)
        {
            return facts[both.resolve(ref)].stats;
        };
        return estimateSelectivity(conjunct, lookup);
    }

    std::vector<NodeEstimate> estimatePlan(const PlanNode& plan, const StatsProvider& stats)
    {
        Walk walk{stats, {}, {}};
        estimateNode(plan, walk);
        return walk.estimates;
    }
} // namespace db::opt
