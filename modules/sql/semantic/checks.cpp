// Name resolution, type checks and aggregate rules for columns, aggregate
// calls and conditions (§M3.3). See checks.h for the contract of each
// function.

#include "semantic/checks.h"

#include <db/sql/ast.h>
#include <db/sql/source_pos.h>

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/common/value.h>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace db::sql
{

    namespace
    {

        // `COUNT`, `AVG`: the function as SQL writes it, for error messages.
        // Common's names are lower case a..z only (`count`), so each letter
        // moves to its capital.
        std::string functionKeyword(AggregateFunction function)
        {
            std::string text(aggregateFunctionName(function));
            for (char& c : text)
            {
                c = static_cast<char>(c - 'a' + 'A');
            }
            return text;
        }

        // `COUNT(*)`, `MAX(e.did)`: an aggregate call as the user wrote it,
        // for error messages.
        std::string callText(const AggregateCall& call)
        {
            std::string text = functionKeyword(call.function) + "(";
            if (call.argument)
            {
                text += call.argument->qualifier ? *call.argument->qualifier + "." : "";
                text += call.argument->name;
            }
            else
            {
                text += "*";
            }
            return text + ")";
        }

        // The output name of an aggregate in a HAVING condition: the entry
        // that already computes the same call (same function, same resolved
        // column) gives its name, so `HAVING COUNT(*) > 5` becomes `n > 5`
        // when the select list has `COUNT(*) AS n` (example 5.4). A call not
        // computed yet is added with its generated name (D2); the project
        // later leaves it out.
        std::string outputNameFor(Grouping& grouping, const Aggregate& aggregate)
        {
            for (const Aggregate& entry : grouping.aggregates)
            {
                if (entry.function == aggregate.function && entry.column == aggregate.column)
                {
                    return entry.alias;
                }
            }
            Aggregate added = aggregate;
            added.alias = aggregateOutputName(aggregate);
            grouping.aggregates.push_back(added);
            return added.alias;
        }

        // One side of a comparison as a plan operand; `type` gets its type.
        db::Operand convertOperand(const Operand& operand, const Schema& scope, Grouping* grouping, Type& type)
        {
            if (const ColumnName* name = std::get_if<ColumnName>(&operand))
            {
                const Column column = resolveColumn(*name, scope);
                if (grouping != nullptr)
                {
                    requireGroupingColumn(*grouping, column, name->pos);
                }
                type = column.type;
                return refOf(column);
            }
            if (const Literal* literal = std::get_if<Literal>(&operand))
            {
                type = literal->value.type();
                return literal->value;
            }
            const AggregateCall& call = std::get<AggregateCall>(operand);
            // WHERE and ON run below the group_by (§M3.5 steps 1-2), so no
            // group exists there to aggregate over (§3.1, required case 7).
            if (grouping == nullptr)
            {
                failAt(ErrorCategory::Aggregate,
                       "Aggregate " + callText(call) + " is not allowed in WHERE or ON: they run before grouping.",
                       call.pos);
            }
            const Aggregate aggregate = checkAggregate(call, scope, type);
            return ColumnRef{std::nullopt, outputNameFor(*grouping, aggregate)};
        }

        // A comparison: both operands, then the type rule (§3.2: "Comparison
        // between different types is an error (including equality)"; INT
        // with REAL is allowed, DECISIONS 2026-11-07). Not recursive, so its
        // locals never pile up on the stack.
        void convertComparison(const SqlCondition& condition, const Schema& scope, Grouping* grouping, Condition& out)
        {
            Type leftType = Type::Int;
            Type rightType = Type::Int;
            out.kind = ConditionKind::Compare;
            out.op = condition.op;
            out.left = convertOperand(condition.left, scope, grouping, leftType);
            out.right = convertOperand(condition.right, scope, grouping, rightType);
            // The same message as common's condition binder, which exec uses,
            // so sql and exec word this error alike.
            if (!typesComparable(leftType, rightType))
            {
                failAt(ErrorCategory::Type,
                       "Incompatible comparison operand types: " + std::string(typeName(leftType)) + " and " +
                           std::string(typeName(rightType)) + ".",
                       condition.pos);
            }
        }

    } // namespace

    void failAt(ErrorCategory category, const std::string& message, SourcePos pos)
    {
        throw Error(category, message, pos.line, pos.column);
    }

    ColumnRef refOf(const Column& column)
    {
        if (column.qualifier.empty())
        {
            return ColumnRef{std::nullopt, column.name};
        }
        return ColumnRef{column.qualifier, column.name};
    }

    Column resolveColumn(const ColumnName& name, const Schema& scope)
    {
        if (name.qualifier)
        {
            // Two steps, so the message says which part failed (§M3.3).
            const std::string written = *name.qualifier + "." + name.name;
            bool qualifierFound = false;
            for (const Column& column : scope.columns())
            {
                if (column.qualifier == *name.qualifier)
                {
                    qualifierFound = true;
                    if (column.name == name.name)
                    {
                        return column;
                    }
                }
            }
            if (!qualifierFound)
            {
                failAt(ErrorCategory::Name,
                       "Unknown table or alias '" + *name.qualifier + "' in '" + written + "'.", name.pos);
            }
            failAt(ErrorCategory::Name, "Table '" + *name.qualifier + "' has no column '" + name.name + "'.",
                   name.pos);
        }
        // A bare name: common's rule (exactly one match; none is Name, two or
        // more is Ambiguity listing the candidates), plus our position.
        try
        {
            return scope.column(scope.resolve(ColumnRef{std::nullopt, name.name}));
        }
        catch (const Error& error)
        {
            failAt(error.category(), error.message(), name.pos);
        }
    }

    void requireGroupingColumn(const Grouping& grouping, const Column& column, SourcePos pos)
    {
        // Resolved columns are compared, never spellings: `did` and
        // `Emp.did` are the same grouping column.
        for (const Column& key : grouping.keys)
        {
            if (key.qualifiedName() == column.qualifiedName())
            {
                return;
            }
        }
        failAt(ErrorCategory::Aggregate,
               "Column '" + column.qualifiedName() + "' must appear in GROUP BY or be used in an aggregate.", pos);
    }

    Aggregate checkAggregate(const AggregateCall& call, const Schema& scope, Type& resultType)
    {
        Aggregate aggregate{call.function, std::nullopt, ""};
        resultType = Type::Int; // COUNT and SUM
        if (!call.argument)
        {
            // The grammar allows `*` in every aggregate; only COUNT(*) means
            // something (§3.2).
            if (call.function != AggregateFunction::Count)
            {
                failAt(ErrorCategory::Type, "Only COUNT may use '*', not " + callText(call) + ".", call.pos);
            }
            return aggregate;
        }
        const Column argument = resolveColumn(*call.argument, scope);
        aggregate.column = refOf(argument);
        switch (call.function)
        {
            case AggregateFunction::Sum:
            case AggregateFunction::Avg:
                // §3.2: "SUM and AVG require an INT column."
                // The same message as common's group_by output schema
                // (plan.cpp), so the rule reads alike wherever it is caught.
                if (argument.type != Type::Int)
                {
                    std::string type(typeName(argument.type));
                    if (argument.type == Type::Varchar)
                    {
                        type += "(" + std::to_string(argument.varcharLength) + ")";
                    }
                    failAt(ErrorCategory::Type,
                           "Aggregate '" + std::string(aggregateFunctionName(call.function)) +
                               "' needs an INT column, but '" + argument.qualifiedName() + "' is " + type + ".",
                           call.pos);
                }
                // AVG divides, so its result is REAL (common's group_by rule).
                if (call.function == AggregateFunction::Avg)
                {
                    resultType = Type::Real;
                }
                break;
            case AggregateFunction::Min:
            case AggregateFunction::Max:
                // MIN and MAX return one of the values, so they keep its type.
                resultType = argument.type;
                break;
            case AggregateFunction::Count:
                break;
        }
        return aggregate;
    }

    // Recursive: one call per AND, OR and NOT. It only holds references and
    // builds each child straight into its slot of `out.children` (on the
    // heap), so 1000 levels fit on the stack (kMaxNestingDepth).
    void convertCondition(const SqlCondition& condition, const Schema& scope, Grouping* grouping, Condition& out)
    {
        if (condition.kind == ConditionKind::Compare)
        {
            convertComparison(condition, scope, grouping, out);
            return;
        }
        // And, Or, Not: the same kind and the same children in the same
        // order. Nothing is split, reordered or simplified (§M3.5 step 2).
        out.kind = condition.kind;
        out.children.resize(condition.children.size());
        for (std::size_t i = 0; i < condition.children.size(); ++i)
        {
            convertCondition(condition.children[i], scope, grouping, out.children[i]);
        }
    }

} // namespace db::sql
