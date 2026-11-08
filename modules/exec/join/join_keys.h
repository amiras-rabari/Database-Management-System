#pragma once
// Join keys for the two equality-only joins, sort_merge_join and hash_join
// (SPEC §M2.3: "Equality conditions only"). Private to exec.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"

namespace db::exec
{
    /// The key columns of an equi-join: leftColumns[i] of a left tuple is
    /// compared with rightColumns[i] of a right tuple.
    struct JoinKeys
    {
        std::vector<std::size_t> leftColumns;
        std::vector<std::size_t> rightColumns;
    };

    /// Checks `condition` while the plan is built and returns its key
    /// columns. Throws, naming the node `opName`:
    /// - what bindJoinCondition throws: unknown column -> Error(Name),
    ///   ambiguous -> Error(Ambiguity), INT against VARCHAR -> Error(Type),
    ///   the same qualified name on both sides -> Error(Schema);
    /// - Error(Schema) "Node 'hash_join': condition must be equalities
    ///   between left and right columns." if it is anything else (an
    ///   inequality, a constant, an OR, a column compared with its own side);
    /// - Error(Type) if two key columns have different types (INT against
    ///   REAL): equal keys must sort and hash the same way.
    /// Example: `R.b = S.b and R.a = S.c` -> left {1, 0}, right {1, 2}.
    JoinKeys equiJoinKeyColumns(const Condition& condition, const Schema& left, const Schema& right,
                                const std::string& opName);

    /// Compares the key of `a` (its columns `aColumns`) with the key of `b`
    /// (its columns `bColumns`), pair by pair: negative if `a` comes first,
    /// 0 if equal, positive if `b` comes first. Adds 1 to `comparisons`,
    /// however many columns it looks at (chunk 07 Q2 (a)).
    int compareKeys(const Tuple& a, const std::vector<std::size_t>& aColumns, const Tuple& b,
                    const std::vector<std::size_t>& bColumns, std::uint64_t& comparisons);

    /// Sort keys for `columns` of `schema`, all ascending: both inputs of a
    /// sort-merge join are sorted on their key columns in pair order.
    std::vector<SortKey> ascendingKeys(const Schema& schema, const std::vector<std::size_t>& columns);
} // namespace db::exec
