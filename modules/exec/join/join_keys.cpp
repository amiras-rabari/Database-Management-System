#include "join/join_keys.h"

#include <optional>
#include <utility>

#include "db/common/error.h"
#include "db/common/evaluator.h"
#include "db/common/value.h"

namespace db::exec
{
    JoinKeys equiJoinKeyColumns(const Condition& condition, const Schema& left, const Schema& right,
                                const std::string& opName)
    {
        // Binding checks names, ambiguity, INT vs VARCHAR and duplicate
        // names, with the same messages as every other operator.
        bindJoinCondition(condition, left, right);

        const std::optional<std::vector<std::pair<std::size_t, std::size_t>>> pairs =
            equiJoinKeys(condition, left, right);
        if (!pairs)
            throw Error(ErrorCategory::Schema,
                        "Node '" + opName + "': condition must be equalities between left and right columns.");

        JoinKeys keys;
        for (const auto& [l, r] : *pairs)
        {
            const Column& leftColumn = left.columns()[l];
            const Column& rightColumn = right.columns()[r];
            if (leftColumn.type != rightColumn.type)
                throw Error(ErrorCategory::Type, "Node '" + opName + "': key columns " + leftColumn.qualifiedName() +
                                                     " and " + rightColumn.qualifiedName() +
                                                     " have different types.");
            keys.leftColumns.push_back(l);
            keys.rightColumns.push_back(r);
        }
        return keys;
    }

    int compareKeys(const Tuple& a, const std::vector<std::size_t>& aColumns, const Tuple& b,
                    const std::vector<std::size_t>& bColumns, std::uint64_t& comparisons)
    {
        ++comparisons;
        for (std::size_t i = 0; i < aColumns.size(); ++i)
        {
            const int order = compareValues(a[aColumns[i]], b[bColumns[i]]);
            if (order != 0)
                return order;
        }
        return 0;
    }

    std::vector<SortKey> ascendingKeys(const Schema& schema, const std::vector<std::size_t>& columns)
    {
        std::vector<SortKey> keys;
        for (std::size_t c : columns)
        {
            const Column& column = schema.columns()[c];
            keys.push_back(SortKey{ColumnRef{column.qualifier, column.name}, true});
        }
        return keys;
    }
} // namespace db::exec
