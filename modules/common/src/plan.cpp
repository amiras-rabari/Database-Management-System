#include "db/common/plan.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/common/error.h"
#include "db/common/evaluator.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace db
{

    namespace
    {

        PlanNode unary(PlanOp op, PlanNode child)
        {
            PlanNode node;
            node.op = op;
            node.children.push_back(std::move(child));
            return node;
        }

        PlanNode binary(PlanOp op, PlanNode left, PlanNode right)
        {
            PlanNode node;
            node.op = op;
            node.children.push_back(std::move(left));
            node.children.push_back(std::move(right));
            return node;
        }

        std::size_t childCount(PlanOp op)
        {
            switch (op)
            {
                case PlanOp::Scan:
                case PlanOp::SeqScan:
                    return 0;
                case PlanOp::Select:
                case PlanOp::Project:
                case PlanOp::Rename:
                case PlanOp::Distinct:
                case PlanOp::Filter:
                case PlanOp::Sort:
                case PlanOp::GroupBy:
                case PlanOp::Limit:
                    return 1;
                case PlanOp::Join:
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                case PlanOp::NestedLoopJoin:
                case PlanOp::SortMergeJoin:
                case PlanOp::HashJoin:
                case PlanOp::UnionAll:
                    return 2;
            }
            throw std::logic_error("childCount: unknown PlanOp value");
        }

        // The ops that exist only in physical plans. toString and
        // outputSchema below read logical plans only. Sort, GroupBy,
        // UnionAll and Limit are in both kinds (sql SPEC §M3.5).
        bool isPhysicalOnly(PlanOp op)
        {
            switch (op)
            {
                case PlanOp::SeqScan:
                case PlanOp::Filter:
                case PlanOp::NestedLoopJoin:
                case PlanOp::SortMergeJoin:
                case PlanOp::HashJoin:
                    return true;
                default:
                    return false;
            }
        }

        bool hasCondition(PlanOp op)
        {
            return op == PlanOp::Select || op == PlanOp::Join;
        }

        // A malformed tree can only come from code that built it by hand, so
        // it is an internal bug, not a user error.
        void checkShape(const PlanNode& node)
        {
            if (isPhysicalOnly(node.op))
            {
                throw std::logic_error("plan node '" + std::string(opName(node.op)) +
                                       "' is physical; this function reads logical plans only");
            }
            if (node.children.size() != childCount(node.op))
            {
                throw std::logic_error("plan node '" + std::string(opName(node.op)) + "' has " +
                                       std::to_string(node.children.size()) + " children");
            }
            if (hasCondition(node.op) && !node.condition)
            {
                throw std::logic_error("plan node '" + std::string(opName(node.op)) + "' has no condition");
            }
        }

        std::string columnList(const std::vector<ColumnRef>& columns)
        {
            std::string out;
            for (const ColumnRef& column : columns)
            {
                if (!out.empty())
                {
                    out += ", ";
                }
                out += column.toString();
            }
            return out;
        }

        // `count(*) as n, avg(R.b) as m`: a group_by's aggregates for toString.
        std::string aggregateList(const std::vector<Aggregate>& aggregates)
        {
            std::string out;
            for (const Aggregate& aggregate : aggregates)
            {
                if (!out.empty())
                {
                    out += ", ";
                }
                out += std::string(aggregateFunctionName(aggregate.function)) + "(" +
                       (aggregate.column ? aggregate.column->toString() : "*") + ") as " + aggregate.alias;
            }
            return out;
        }

        // `n desc, R.a asc`: a sort's keys for toString.
        std::string sortKeyList(const std::vector<SortKey>& keys)
        {
            std::string out;
            for (const SortKey& key : keys)
            {
                if (!out.empty())
                {
                    out += ", ";
                }
                out += key.column.toString() + (key.ascending ? " asc" : " desc");
            }
            return out;
        }

        // A column type as SQL writes it, for error messages: INT,
        // VARCHAR(20), REAL.
        std::string typeText(const Column& column)
        {
            switch (column.type)
            {
                case Type::Int:
                    return "INT";
                case Type::Varchar:
                    return "VARCHAR(" + std::to_string(column.varcharLength) + ")";
                case Type::Real:
                    return "REAL";
            }
            throw std::logic_error("typeText: unknown Type value");
        }

        // The output schema of a logical group_by over a child with schema
        // `input` (example 5.4: keys ["Emp.did"], aggregates count(*) as n and
        // avg(Emp.salary) as avg_sal give Emp.did INT, n INT, avg_sal REAL).
        Schema groupBySchema(const PlanNode& plan, const Schema& input)
        {
            std::vector<Column> columns;
            // 1. The keys, copied from the child (qualified as they are there).
            for (const ColumnRef& key : plan.columns)
            {
                columns.push_back(input.column(input.resolve(key)));
            }
            // 2. One column per aggregate. It has no qualifier: nodes above
            //    name it by the alias alone (`n`), and a rename above
            //    qualifies it (`t.n`, example 5.6).
            for (const Aggregate& aggregate : plan.aggregates)
            {
                Column output{"", aggregate.alias, Type::Int, 0};
                if (aggregate.column)
                {
                    const Column& argument = input.column(input.resolve(*aggregate.column));
                    const bool needsInt =
                        aggregate.function == AggregateFunction::Sum || aggregate.function == AggregateFunction::Avg;
                    // sql §M3.3: "SUM and AVG require an INT column".
                    if (needsInt && argument.type != Type::Int)
                    {
                        throw Error(ErrorCategory::Type, "Aggregate '" +
                                                             std::string(aggregateFunctionName(aggregate.function)) +
                                                             "' needs an INT column, but '" +
                                                             argument.qualifiedName() + "' is " + typeText(argument) +
                                                             ".");
                    }
                    // MIN and MAX return one of the input values, so they
                    // keep its type and VARCHAR length.
                    if (aggregate.function == AggregateFunction::Min || aggregate.function == AggregateFunction::Max)
                    {
                        output.type = argument.type;
                        output.varcharLength = argument.varcharLength;
                    }
                }
                // COUNT counts rows and SUM adds INTs: both INT (the Column
                // default above). AVG divides, so its result is REAL (exec
                // §M2.3, DECISIONS 2026-11-07 "REAL").
                if (aggregate.function == AggregateFunction::Avg)
                {
                    output.type = Type::Real;
                }
                columns.push_back(output);
            }
            // The Schema constructor rejects two columns with one name
            // (e.g. two aggregates both called n) with Error(Schema).
            return Schema(std::move(columns));
        }

    } // namespace

    std::string_view aggregateFunctionName(AggregateFunction function)
    {
        switch (function)
        {
            case AggregateFunction::Count:
                return "count";
            case AggregateFunction::Sum:
                return "sum";
            case AggregateFunction::Avg:
                return "avg";
            case AggregateFunction::Min:
                return "min";
            case AggregateFunction::Max:
                return "max";
        }
        throw std::logic_error("aggregateFunctionName: unknown AggregateFunction value");
    }

    // Built from the spec's example (§M2.3): count(*) -> count_star,
    // avg(R.salary) -> avg_R_salary. The dot becomes an underscore, so the
    // name is one plain word that a bare reference finds.
    std::string aggregateOutputName(const Aggregate& aggregate)
    {
        std::string name(aggregateFunctionName(aggregate.function));
        if (!aggregate.column)
            return name + "_star";
        if (aggregate.column->qualifier)
            name += "_" + *aggregate.column->qualifier;
        return name + "_" + aggregate.column->name;
    }

    std::string_view opName(PlanOp op)
    {
        switch (op)
        {
            case PlanOp::Scan:
                return "scan";
            case PlanOp::Select:
                return "select";
            case PlanOp::Project:
                return "project";
            case PlanOp::Join:
                return "join";
            case PlanOp::Cross:
                return "cross";
            case PlanOp::Rename:
                return "rename";
            case PlanOp::Union:
                return "union";
            case PlanOp::Intersect:
                return "intersect";
            case PlanOp::Except:
                return "except";
            case PlanOp::Distinct:
                return "distinct";
            case PlanOp::SeqScan:
                return "seqscan";
            case PlanOp::Filter:
                return "filter";
            case PlanOp::Sort:
                return "sort";
            case PlanOp::GroupBy:
                return "group_by";
            case PlanOp::NestedLoopJoin:
                return "nested_loop_join";
            case PlanOp::SortMergeJoin:
                return "sort_merge_join";
            case PlanOp::HashJoin:
                return "hash_join";
            case PlanOp::UnionAll:
                return "union_all";
            case PlanOp::Limit:
                return "limit";
        }
        throw std::logic_error("opName: unknown PlanOp value");
    }

    PlanNode PlanNode::scan(std::string table)
    {
        PlanNode node;
        node.op = PlanOp::Scan;
        node.table = std::move(table);
        return node;
    }

    PlanNode PlanNode::select(Condition condition, PlanNode child)
    {
        PlanNode node = unary(PlanOp::Select, std::move(child));
        node.condition = std::move(condition);
        return node;
    }

    PlanNode PlanNode::project(std::vector<ColumnRef> columns, PlanNode child)
    {
        PlanNode node = unary(PlanOp::Project, std::move(child));
        node.columns = std::move(columns);
        return node;
    }

    PlanNode PlanNode::join(Condition condition, PlanNode left, PlanNode right)
    {
        PlanNode node = binary(PlanOp::Join, std::move(left), std::move(right));
        node.condition = std::move(condition);
        return node;
    }

    PlanNode PlanNode::cross(PlanNode left, PlanNode right)
    {
        return binary(PlanOp::Cross, std::move(left), std::move(right));
    }

    PlanNode PlanNode::rename(std::string alias, PlanNode child)
    {
        PlanNode node = unary(PlanOp::Rename, std::move(child));
        node.alias = std::move(alias);
        return node;
    }

    PlanNode PlanNode::unionOf(PlanNode left, PlanNode right)
    {
        return binary(PlanOp::Union, std::move(left), std::move(right));
    }

    PlanNode PlanNode::intersect(PlanNode left, PlanNode right)
    {
        return binary(PlanOp::Intersect, std::move(left), std::move(right));
    }

    PlanNode PlanNode::except(PlanNode left, PlanNode right)
    {
        return binary(PlanOp::Except, std::move(left), std::move(right));
    }

    PlanNode PlanNode::distinct(PlanNode child)
    {
        return unary(PlanOp::Distinct, std::move(child));
    }

    PlanNode PlanNode::groupBy(std::vector<ColumnRef> keys, std::vector<Aggregate> aggregates, PlanNode child)
    {
        PlanNode node = unary(PlanOp::GroupBy, std::move(child));
        node.columns = std::move(keys);
        node.aggregates = std::move(aggregates);
        return node;
    }

    PlanNode PlanNode::sort(std::vector<SortKey> keys, PlanNode child)
    {
        PlanNode node = unary(PlanOp::Sort, std::move(child));
        node.sortKeys = std::move(keys);
        return node;
    }

    PlanNode PlanNode::limit(std::int32_t count, PlanNode child)
    {
        PlanNode node = unary(PlanOp::Limit, std::move(child));
        node.limitCount = count;
        return node;
    }

    PlanNode PlanNode::unionAll(PlanNode left, PlanNode right)
    {
        return binary(PlanOp::UnionAll, std::move(left), std::move(right));
    }

    bool operator==(const PlanNode& left, const PlanNode& right)
    {
        if (left.op != right.op || left.children != right.children)
        {
            return false;
        }
        switch (left.op)
        {
            case PlanOp::Scan:
            case PlanOp::SeqScan:
                return left.table == right.table;
            case PlanOp::Select:
            case PlanOp::Join:
            case PlanOp::Filter:
            case PlanOp::NestedLoopJoin:
            case PlanOp::SortMergeJoin:
            case PlanOp::HashJoin:
                return left.condition == right.condition;
            case PlanOp::Project:
                return left.columns == right.columns;
            case PlanOp::Rename:
                return left.alias == right.alias;
            case PlanOp::Distinct:
                return left.mode == right.mode;
            case PlanOp::Sort:
                return left.sortKeys == right.sortKeys;
            case PlanOp::GroupBy:
                return left.mode == right.mode && left.columns == right.columns &&
                       left.aggregates == right.aggregates;
            case PlanOp::Limit:
                return left.limitCount == right.limitCount;
            case PlanOp::Cross:
            case PlanOp::Union:
            case PlanOp::Intersect:
            case PlanOp::Except:
            case PlanOp::UnionAll:
                return true;
        }
        throw std::logic_error("operator==: unknown PlanOp value");
    }

    std::string toString(const PlanNode& plan, ConditionStyle style)
    {
        checkShape(plan);
        std::string out(opName(plan.op));
        switch (plan.op)
        {
            case PlanOp::Scan:
                return out + "(" + plan.table + ")";
            case PlanOp::Select:
            case PlanOp::Join:
                out += "[" + toString(*plan.condition, style) + "]";
                break;
            case PlanOp::Project:
                out += "[" + columnList(plan.columns) + "]";
                break;
            case PlanOp::Rename:
                out += "[" + plan.alias + "]";
                break;
            case PlanOp::GroupBy:
                // `;` separates the keys from the aggregates, so an empty
                // list on either side is still visible: `group_by[; count(*) as n]`.
                out += "[" + columnList(plan.columns) + "; " + aggregateList(plan.aggregates) + "]";
                break;
            case PlanOp::Sort:
                out += "[" + sortKeyList(plan.sortKeys) + "]";
                break;
            case PlanOp::Limit:
                out += "[" + std::to_string(plan.limitCount) + "]";
                break;
            case PlanOp::Cross:
            case PlanOp::Union:
            case PlanOp::UnionAll:
            case PlanOp::Intersect:
            case PlanOp::Except:
            case PlanOp::Distinct:
                break;
            default:
                break; // physical-only ops: checkShape above already threw
        }
        out += "(" + toString(plan.children[0], style);
        if (plan.children.size() == 2)
        {
            out += ", " + toString(plan.children[1], style);
        }
        return out + ")";
    }


    //// const std::function<std::optional<Schema>(const std::string&)>& tableSchema means
    ///“a function you give me: I pass in a table name, and you give back that table’s schema,
    ///or nothing if no such table exists.
    Schema outputSchema(const PlanNode& plan,
                        const std::function<std::optional<Schema>(const std::string&)>& tableSchema)
    {
        checkShape(plan);
        if (plan.op == PlanOp::Scan)
        {
            std::optional<Schema> schema = tableSchema(plan.table);
            if (!schema)
            {
                throw Error(ErrorCategory::Name, "Unknown relation '" + plan.table + "'.");
            }
            return std::move(*schema);
        }

        const Schema first = outputSchema(plan.children[0], tableSchema);
        switch (plan.op)
        {
            case PlanOp::Select:
                bindCondition(*plan.condition, first);
                return first;
            case PlanOp::Project:
                return projectSchema(first, plan.columns);
            case PlanOp::Rename:
                return requalify(first, plan.alias);
            case PlanOp::Distinct:
            case PlanOp::Limit:
                return first;
            case PlanOp::Sort:
                // Sorting changes the order only; each key must name a column.
                for (const SortKey& key : plan.sortKeys)
                {
                    first.resolve(key.column);
                }
                return first;
            case PlanOp::GroupBy:
                return groupBySchema(plan, first);
            default:
                break;
        }

        const Schema second = outputSchema(plan.children[1], tableSchema);
        switch (plan.op)
        {
            case PlanOp::Join:
            {
                Schema combined = concatSchemas(first, second);
                bindJoinCondition(*plan.condition, first, second);
                return combined;
            }
            case PlanOp::Cross:
                return concatSchemas(first, second);
            case PlanOp::Union:
            case PlanOp::UnionAll:
            case PlanOp::Intersect:
            case PlanOp::Except:
                checkUnionCompatible(first, second, opName(plan.op));
                return first;
            default:
                throw std::logic_error("outputSchema: unknown PlanOp value");
        }
    }

} // namespace db
