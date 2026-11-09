#pragma once

// The semantic rules of §M3.3 for single columns, aggregate calls and
// conditions: name resolution, type checks and the aggregate rules. The
// translator (translate/translate.cpp) calls them while it builds the plan.
//
// "The scope" is always a common Schema: the schema of the FROM plan built
// so far. Each FROM entry's columns are qualified by its key (alias, else
// table name), so `e.name` and `Emp.name` can never be confused.
//
// Every error carries the position of the SQL that caused it (§M3.6).
// Common's own errors have no position, so they are thrown again with one.

#include <db/sql/ast.h>
#include <db/sql/source_pos.h>

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/common/value.h>

#include <string>
#include <vector>

namespace db::sql
{

    /// Throws Error(category, message) at `pos`.
    [[noreturn]] void failAt(ErrorCategory category, const std::string& message, SourcePos pos);

    /// The plan reference of a resolved column: `e.name` (its qualifier is
    /// the scope key), or a bare name when it has no qualifier (an aggregate
    /// output).
    ColumnRef refOf(const Column& column);

    /// Finds the one scope column `name` means (§M3.3):
    /// - `t.c`: Name "Unknown table or alias 't' in 't.c'." when no entry is
    ///   called t, else Name "Table 't' has no column 'c'." when t lacks c.
    /// - `c`: common's Schema::resolve rule, Name "Unknown attribute 'c'." or
    ///   Ambiguity "Ambiguous unqualified attribute 'c' (matches e.c, d.c). …".
    /// All at the column's position.
    Column resolveColumn(const ColumnName& name, const Schema& scope);

    /// What an aggregating block knows while it is translated: the resolved
    /// GROUP BY columns and the group_by's aggregate entries, each with its
    /// output name in `alias` (D2).
    struct Grouping
    {
        std::vector<Column> keys;
        std::vector<Aggregate> aggregates;
    };

    /// Throws Aggregate "Column 'Emp.name' must appear in GROUP BY or be used
    /// in an aggregate." at `pos` unless `column` is one of the keys (§3.1).
    void requireGroupingColumn(const Grouping& grouping, const Column& column, SourcePos pos);

    /// Checks an aggregate call and resolves its argument (§3.2): `*` only in
    /// COUNT, SUM and AVG only over INT (Type errors at the call). Returns
    /// the aggregate without an alias; `resultType` gets its output type:
    /// COUNT and SUM INT, AVG REAL, MIN and MAX the argument's type.
    Aggregate checkAggregate(const AggregateCall& call, const Schema& scope, Type& resultType);

    /// Turns a WHERE, ON or HAVING condition into a plan condition, node for
    /// node, filling `out` (a fresh Condition) in place:
    /// - every column resolved against `scope` and written qualified;
    /// - every comparison type-checked with common's typesComparable
    ///   (Type "Cannot compare INT with VARCHAR." at the comparison);
    /// - `grouping` empty (WHERE, ON): an aggregate is an Aggregate error;
    /// - `grouping` set (HAVING): columns must be grouping columns, and an
    ///   aggregate becomes its output name (`COUNT(*) > 5` → `n > 5`); one
    ///   that is not an entry yet is added with its generated name.
    void convertCondition(const SqlCondition& condition, const Schema& scope, Grouping* grouping, Condition& out);

} // namespace db::sql
