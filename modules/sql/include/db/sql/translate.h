#pragma once

/// @file
/// SQL query in, logical plan out (§M3.3-§M3.5). Names, types and aggregate
/// rules are checked while the plan is built, so the first problem found is
/// thrown with its position in the SQL text.

#include <db/sql/ast.h>
#include <db/sql/table_lookup.h>

#include <db/common/plan.h>

namespace db::sql
{

    /// The logical plan of one parsed query, in the canonical order of
    /// §M3.5, bottom to top:
    /// FROM (scan, rename only when an alias is written, cross for commas,
    /// join with its ON for JOIN) → WHERE `select` → `group_by` (if the block
    /// aggregates) → HAVING `select` → `project` → `distinct` (only for
    /// SELECT DISTINCT without GROUP BY) → `sort` → `limit`.
    /// - Every column is fully qualified (`Emp.name`, `e.name`); only
    ///   aggregate outputs are bare (`n`, `count_star`; D1, D2).
    /// - No condition is moved, split or reordered.
    /// - ORDER BY on a column that is not selected: a wider `project` under
    ///   the `sort`, and a final `project` above it (D3).
    ///
    /// Errors (db::Error with the line and column of the offending part;
    /// checked in plan order, FROM first, so the first one found is thrown):
    /// - Name: unknown table, duplicate table name or alias in FROM, unknown
    ///   table/alias or column, an output alias in HAVING, ORDER BY with
    ///   DISTINCT on a column that is not selected.
    /// - Ambiguity: a bare column found in two FROM entries.
    /// - Type: a comparison of different types (INT with REAL is allowed),
    ///   SUM/AVG over a non-INT column, `*` in an aggregate other than COUNT.
    /// - Aggregate: an aggregate in WHERE or ON; a non-grouping column in the
    ///   select list, HAVING or ORDER BY of an aggregating block.
    /// - Schema: the same output column twice (`SELECT name, name`), which
    ///   exec would refuse too; a FROM subquery whose outputs collide under
    ///   its alias (`t.did` twice), at the alias; set-operator inputs that
    ///   differ in column count or types (common's checkUnionCompatible), at
    ///   the operator keyword.
    /// - Syntax: "Query nested too deep: its plan has more than 1000 levels."
    ///   when the finished plan breaks common's depth rule (limits.h: plan
    ///   nodes with inputs + and/or/not levels, counted as common's JSON
    ///   reader counts the condition text). The parser counts SQL levels, and
    ///   one can become several plan levels (a FROM subquery adds a rename and
    ///   a project), so e.g. 500 nested subqueries are refused here. At the
    ///   query's first SELECT, or at its last set operator keyword.
    ///
    /// A FROM subquery is translated on its own (it sees no outer names),
    /// wrapped in `rename` to its alias, and its output schema, qualified by
    /// the alias, becomes the scope entry (example 5.6: `t.did`, `t.n`).
    /// Set operators become `union`, `union_all`, `intersect` or `except`
    /// over the two sides, left associative (example 5.7). Each select block
    /// keeps its own ORDER BY / LIMIT, inside its side of a set operator.
    PlanNode translateQuery(const QueryExpr& query, const TableLookup& lookup);

} // namespace db::sql
