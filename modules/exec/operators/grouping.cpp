#include "operators/grouping.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "sort/set_ops.h"

namespace db::exec
{
    Grouping makeGrouping(const Schema& child, const std::vector<ColumnRef>& keys,
                          const std::vector<Aggregate>& aggregates)
    {
        Grouping grouping;
        std::vector<Column> output;
        for (const ColumnRef& key : keys)
        {
            grouping.keyColumns.push_back(child.resolve(key));
            output.push_back(child.column(grouping.keyColumns.back()));
        }
        // The state row starts with the same key columns.
        std::vector<Column> state = output;

        for (const Aggregate& aggregate : aggregates)
        {
            const std::string name = aggregateOutputName(aggregate);
            grouping.functions.push_back(aggregate.function);
            grouping.names.push_back(name);
            // count(*) has no column; count never looks at its column's value.
            const std::size_t input = aggregate.column ? child.resolve(*aggregate.column) : 0;
            grouping.inputColumns.push_back(input);

            switch (aggregate.function)
            {
            case AggregateFunction::Count:
                output.push_back(Column{"", name, Type::Int, 0});
                state.push_back(Column{"", name, Type::Int, 0});
                break;
            case AggregateFunction::Sum:
            case AggregateFunction::Avg:
                if (child.column(input).type != Type::Int)
                {
                    throw Error(ErrorCategory::Type, std::string(aggregateFunctionName(aggregate.function)) +
                                "(" + child.column(input).qualifiedName() +
                                ") needs an INT column, but it is " +
                                std::string(typeName(child.column(input).type)) + ".");
                }
                if (aggregate.function == AggregateFunction::Sum)
                {
                    output.push_back(Column{"", name, Type::Int, 0});
                    state.push_back(Column{"", name, Type::Real, 0});
                }
                else
                {
                    output.push_back(Column{"", name, Type::Real, 0});
                    state.push_back(Column{"", name, Type::Real, 0});
                    state.push_back(Column{"", name + "_count", Type::Int, 0});
                }
                break;
            case AggregateFunction::Min:
            case AggregateFunction::Max:
                {
                    // The column's own type; a VARCHAR keeps its length n.
                    Column column = child.column(input);
                    column.qualifier = "";
                    column.name = name;
                    output.push_back(column);
                    state.push_back(column);
                    if (column.type == Type::Varchar)
                        grouping.stateCanGrow = true;
                    break;
                }
            }
        }
        // The output schema first: it reports two equal output names.
        grouping.outputSchema = Schema(std::move(output));
        grouping.stateSchema = Schema(std::move(state));
        return grouping;
    }

    Grouping distinctGrouping(const Schema& child)
    {
        Grouping grouping;
        for (std::size_t i = 0; i < child.size(); ++i)
            grouping.keyColumns.push_back(i);
        grouping.stateSchema = child;
        grouping.outputSchema = child;
        return grouping;
    }

    Tuple toStateRow(const Grouping& grouping, const Tuple& input)
    {
        Tuple state;
        for (std::size_t column : grouping.keyColumns)
            state.push_back(input[column]);
        for (std::size_t a = 0; a < grouping.functions.size(); ++a)
        {
            const Value& value = input[grouping.inputColumns[a]];
            switch (grouping.functions[a])
            {
            case AggregateFunction::Count:
                state.push_back(Value::ofInt(1));
                break;
            case AggregateFunction::Sum:
                state.push_back(Value::ofReal(value.asInt()));
                break;
            case AggregateFunction::Avg:
                state.push_back(Value::ofReal(value.asInt()));
                state.push_back(Value::ofInt(1));
                break;
            case AggregateFunction::Min:
            case AggregateFunction::Max:
                state.push_back(value);
                break;
            }
        }
        return state;
    }

    void mergeStateRow(const Grouping& grouping, Tuple& into, const Tuple& from, std::uint64_t& comparisons)
    {
        // Walk both rows column by column after the key; avg uses two columns.
        std::size_t p = grouping.keyColumns.size();
        for (AggregateFunction function : grouping.functions)
        {
            switch (function)
            {
            case AggregateFunction::Count:
                // A count is at most the number of input rows, which fits an INT.
                into[p] = Value::ofInt(into[p].asInt() + from[p].asInt());
                p += 1;
                break;
            case AggregateFunction::Sum:
                into[p] = Value::ofReal(into[p].asReal() + from[p].asReal());
                p += 1;
                break;
            case AggregateFunction::Avg:
                into[p] = Value::ofReal(into[p].asReal() + from[p].asReal());
                into[p + 1] = Value::ofInt(into[p + 1].asInt() + from[p + 1].asInt());
                p += 2;
                break;
            case AggregateFunction::Min:
            case AggregateFunction::Max:
                {
                    // One comparison per check, like a sort key comparison.
                    ++comparisons;
                    const int order = compareValues(from[p], into[p]);
                    if ((function == AggregateFunction::Min && order < 0) ||
                        (function == AggregateFunction::Max && order > 0))
                        into[p] = from[p];
                    p += 1;
                    break;
                }
            }
        }
    }

    Tuple finishStateRow(const Grouping& grouping, const Tuple& state)
    {
        Tuple output(state.begin(), state.begin() + static_cast<std::ptrdiff_t>(grouping.keyColumns.size()));
        std::size_t p = grouping.keyColumns.size();
        for (std::size_t a = 0; a < grouping.functions.size(); ++a)
        {
            switch (grouping.functions[a])
            {
            case AggregateFunction::Count:
            case AggregateFunction::Min:
            case AggregateFunction::Max:
                output.push_back(state[p]);
                p += 1;
                break;
            case AggregateFunction::Sum:
                {
                    // The total is a whole number held exactly in the double.
                    const double total = state[p].asReal();
                    if (total > std::numeric_limits<std::int32_t>::max() ||
                        total < std::numeric_limits<std::int32_t>::min())
                    {
                        throw Error(ErrorCategory::Type, grouping.names[a] + " = " +
                                    std::to_string(static_cast<long long>(total)) +
                                    " does not fit in INT.");
                    }
                    output.push_back(Value::ofInt(static_cast<std::int32_t>(total)));
                    p += 1;
                    break;
                }
            case AggregateFunction::Avg:
                output.push_back(Value::ofReal(state[p].asReal() / state[p + 1].asInt()));
                p += 2;
                break;
            }
        }
        return output;
    }

    Tuple emptyInputRow(const Grouping& grouping)
    {
        Tuple output;
        for (std::size_t a = 0; a < grouping.functions.size(); ++a)
        {
            if (grouping.functions[a] != AggregateFunction::Count)
            {
                throw Error(ErrorCategory::Aggregate,
                            grouping.names[a] + " is undefined: the input is empty and there are no grouping columns.");
            }
            output.push_back(Value::ofInt(0));
        }
        return output;
    }

    SortGroupByOperator::SortGroupByOperator(ExecContext& ctx, std::unique_ptr<Operator> child, Grouping grouping,
                                             const FrameShare& share)
        : Operator(ctx, oneChild(std::move(child))), grouping_(std::move(grouping)), share_(share)
    {
        setSchema(grouping_.outputSchema);
    }

    void SortGroupByOperator::doOpen()
    {
        sorter_.reset();
        hasCurrent_ = false;
        returnEmptyRow_ = false;
        std::uint64_t& comparisons = mutableCounters().comparisons;
        Tuple t;

        if (grouping_.keyColumns.empty())
        {
            // One group for the whole input: no sort needed.
            child(0).open();
            while (child(0).next(t))
            {
                if (hasCurrent_)
                    mergeStateRow(grouping_, current_, toStateRow(grouping_, t), comparisons);
                else
                    current_ = toStateRow(grouping_, t);
                hasCurrent_ = true;
            }
            child(0).close();
            returnEmptyRow_ = !hasCurrent_;
            return;
        }

        // Sort the input rows on the key columns, ascending (the direction
        // does not matter, only that equal keys end up next to each other).
        const Schema& input = child(0).schema();
        std::vector<SortKey> keys;
        for (std::size_t column : grouping_.keyColumns)
        {
            const Column& c = input.column(column);
            keys.push_back(SortKey{ColumnRef{c.qualifier, c.name}, true});
        }
        // Frames as SortOperator: pass 0 ownFrames, merges openFrames, the
        // final merge streamFrames.
        sorter_.emplace(ctx().engine(), input, keys, share_.ownFrames, share_.openFrames, share_.streamFrames,
                        comparisons);
        child(0).open();
        while (child(0).next(t))
            sorter_->add(t);
        child(0).close();
        sorter_->finishInput();

        // Start the first group. An empty input has no group: zero rows.
        if (sorter_->next(t))
        {
            current_ = toStateRow(grouping_, t);
            hasCurrent_ = true;
        }
    }

    bool SortGroupByOperator::doNext(Tuple& out)
    {
        if (returnEmptyRow_)
        {
            returnEmptyRow_ = false;
            out = emptyInputRow(grouping_);
            return true;
        }
        if (!hasCurrent_)
            return false;

        std::uint64_t& comparisons = mutableCounters().comparisons;
        if (sorter_)
        {
            // Add rows while the key stays the same; a new key ends the group.
            Tuple t;
            while (sorter_->next(t))
            {
                Tuple state = toStateRow(grouping_, t);
                if (compareFirstColumns(state, current_, grouping_.keyColumns.size(), comparisons) == 0)
                {
                    mergeStateRow(grouping_, current_, state, comparisons);
                    continue;
                }
                out = finishStateRow(grouping_, current_);
                current_ = std::move(state);
                return true;
            }
        }
        // The input has ended: the last group.
        out = finishStateRow(grouping_, current_);
        hasCurrent_ = false;
        return true;
    }

    void SortGroupByOperator::doClose()
    {
        // Deletes the runs and gives back the sorter's frames.
        sorter_.reset();
    }
} // namespace db::exec
