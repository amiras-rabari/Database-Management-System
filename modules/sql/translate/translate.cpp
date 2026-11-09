// The translator: one select block in, its logical plan out, built bottom
// to top in the canonical order of §M3.5. Every name, type and aggregate
// rule is checked (semantic/checks.h) at the moment its node is built, so
// errors come in plan order: FROM, WHERE, GROUP BY and the select list,
// HAVING, ORDER BY.
//
// The translator makes no optimisation decisions: each condition goes on
// the node its clause belongs to, unchanged (example 5.2's note).

#include <db/sql/translate.h>

#include "semantic/checks.h"

#include <db/sql/ast.h>
#include <db/sql/source_pos.h>
#include <db/sql/table_lookup.h>

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/limits.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/common/value.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace db::sql
{

    namespace
    {

        // Stack size. Queries nest: a FROM subquery is a whole query again,
        // and a set operator has a query on each side. So these functions
        // call each other recursively:
        //   translateInto -> translateSelectBlock -> translateFrom
        //     -> translateFromEntry -> translateInto (a FROM subquery)
        //   translateInto -> translateInto (each side of a set operator)
        // The parser allows 1000 such levels (kMaxNestingDepth), so these
        // functions fill a caller-owned PlanNode in place and keep only small
        // locals. The bigger work (lookups, conditions, the clauses after
        // FROM) happens in helpers that are not on that path, so their locals
        // are gone again before the next level starts.

        void translateInto(const QueryExpr& query, const TableLookup& lookup, PlanNode& out);

        // §M3.3: a FROM entry's key is its alias if written, else the table
        // name, and two entries may not share one (case 12).
        void addKey(const TableRef& table, std::vector<std::string>& keys)
        {
            const Name& key = table.alias ? *table.alias : table.table;
            for (const std::string& earlier : keys)
            {
                if (earlier == key.text)
                {
                    failAt(ErrorCategory::Name, "Table name or alias '" + key.text + "' is used twice in FROM.",
                           key.pos);
                }
            }
            keys.push_back(key.text);
        }

        // A base table: `scan`, inside a `rename` only when an alias was
        // written (5.1 has none, 5.2 has two). Its columns are qualified by
        // the key, so after `FROM Emp e` only `e.name` exists, never
        // `Emp.name`.
        void translateBaseTable(const TableRef& table, const TableLookup& lookup, PlanNode& entry, Schema& schema)
        {
            const std::optional<Schema> found = lookup(table.table.text);
            if (!found)
            {
                failAt(ErrorCategory::Name, "Unknown table '" + table.table.text + "'.", table.table.pos);
            }
            entry = PlanNode::scan(table.table.text);
            if (table.alias)
            {
                entry = PlanNode::rename(table.alias->text, std::move(entry));
            }
            schema = requalify(*found, table.alias ? table.alias->text : table.table.text);
        }

        // The scope columns of a FROM subquery: its output schema (common's
        // outputSchema), qualified by the alias. So `Emp.did` and the bare
        // aggregate output `n` become `t.did` and `t.n` (example 5.6), and the
        // outer query sees nothing else. Two outputs with one name (`e.did`,
        // `d.did`) would both become `t.did`: Schema refuses that, and the
        // error gets the alias's position (decision (a)).
        void subqueryScope(const PlanNode& subplan, const TableLookup& lookup, const Name& alias, Schema& schema)
        {
            const Schema inner = outputSchema(subplan, lookup);
            try
            {
                schema = requalify(inner, alias.text);
            }
            catch (const Error& error)
            {
                failAt(error.category(), error.message(), alias.pos);
            }
        }

        // One FROM entry: its plan leaf in `entry`, its scope columns in
        // `schema`. A subquery is translated on its own, with nothing of the
        // outer query in scope (the grammar has no correlated subqueries),
        // then wrapped in a rename (example 5.6). The rename node is filled
        // in place, so the subquery is built straight into its child slot.
        void translateFromEntry(const TableRef& table, const TableLookup& lookup, PlanNode& entry, Schema& schema)
        {
            if (table.subquery.empty())
            {
                translateBaseTable(table, lookup, entry, schema);
                return;
            }
            entry.op = PlanOp::Rename; // rename[t](subquery plan)
            entry.alias = table.alias->text;
            entry.children.resize(1);
            translateInto(table.subquery[0], lookup, entry.children[0]);
            subqueryScope(entry.children[0], lookup, *table.alias, schema);
        }

        // After the i-th entry (i >= 1) was built as the right input of
        // `out`, a cross node: the scope grows by its columns, and for a JOIN
        // chain the node becomes a join with its ON condition. The ON sees
        // tables 0..i only, the combined schema so far (D5).
        void finishFromStep(const FromClause& from, std::size_t i, const Schema& entrySchema, PlanNode& out,
                            Schema& scope)
        {
            // Keys are unique, so no qualified name occurs twice here.
            scope = concatSchemas(scope, entrySchema);
            if (!from.onConditions.empty())
            {
                Condition on;
                convertCondition(from.onConditions[i - 1], scope, nullptr, on);
                out.op = PlanOp::Join;
                out.condition = std::move(on);
            }
        }

        // §M3.5 step 1: scans, renames for aliases, then cross products
        // (comma list) or joins (JOIN chain), left to right.
        // Example: `FROM Emp e JOIN Dept d ON e.did = d.did` gives
        // join[e.did = d.did](rename[e](scan(Emp)), rename[d](scan(Dept))).
        // `out` gets the plan and `scope` its schema, which every later
        // clause resolves against. A loop: the tree grows left-deep around
        // "the plan so far" (cross(plan so far, next entry)).
        void translateFrom(const FromClause& from, const TableLookup& lookup, PlanNode& out, Schema& scope)
        {
            std::vector<std::string> keys; // the scope keys seen so far
            for (std::size_t i = 0; i < from.tables.size(); ++i)
            {
                addKey(from.tables[i], keys);
                if (i == 0)
                {
                    translateFromEntry(from.tables[0], lookup, out, scope);
                    continue;
                }
                // Wrap the plan so far as the left input; the new entry is
                // built straight into the right input.
                out = PlanNode::cross(std::move(out), PlanNode());
                Schema entrySchema;
                translateFromEntry(from.tables[i], lookup, out.children[1], entrySchema);
                finishFromStep(from, i, entrySchema, out, scope);
            }
        }

        // §3.1: "A query is aggregating if it has a GROUP BY or an aggregate
        // anywhere in its select list."
        bool isAggregating(const SelectBlock& block)
        {
            if (!block.groupBy.empty())
            {
                return true;
            }
            for (const SelectItem& item : block.items)
            {
                if (std::holds_alternative<AggregateCall>(item.expression))
                {
                    return true;
                }
            }
            return false;
        }

        // The project's columns, and next to each the name ORDER BY may use
        // for it (§M3.3 "output aliases"): an `AS` alias, or an aggregate's
        // output name; empty for a plain column without alias.
        struct OutputList
        {
            std::vector<ColumnRef> columns;
            std::vector<std::string> sortNames;
        };

        // Adds one output column. The same column twice is a schema error at
        // the second one, as exec refuses such a project too (decision (a)).
        void addOutput(OutputList& outputs, ColumnRef column, std::string sortName, SourcePos pos)
        {
            for (const ColumnRef& earlier : outputs.columns)
            {
                if (earlier == column)
                {
                    failAt(ErrorCategory::Schema, "Duplicate output column '" + column.toString() + "'.", pos);
                }
            }
            outputs.columns.push_back(std::move(column));
            outputs.sortNames.push_back(std::move(sortName));
        }

        // The same aggregate selected twice under two aliases
        // (`COUNT(*) AS a, COUNT(*) AS b`) is a schema error. The logical
        // plan could hold it, but a physical plan names an aggregate by
        // common's rule only (both would be `count_star`), and exec refuses
        // two columns with one name (§M2.1), so the plan could never run
        // (§M3.10 "plans load and run"). Same reason as the duplicate output
        // column rule above. DECISIONS 2026-11-09.
        void refuseSameAggregate(const Grouping& grouping, const Aggregate& aggregate, SourcePos pos)
        {
            const std::string name = aggregateOutputName(aggregate);
            for (const Aggregate& earlier : grouping.aggregates)
            {
                if (aggregateOutputName(earlier) == name)
                {
                    failAt(ErrorCategory::Schema,
                           "Aggregates '" + earlier.alias + "' and '" + aggregate.alias + "' are the same aggregate (" +
                               name + "); select it once.",
                           pos);
                }
            }
        }

        // The select list (§M3.5 step 5's columns). `grouping` is set for an
        // aggregating block: then each plain column must be a grouping
        // column (case 6), and each aggregate becomes a group_by entry named
        // by its alias or common's generated name (D2), shown bare (D1).
        // SELECT * expands to every scope column, FROM order then schema
        // order (§M3.3, case 2).
        void translateSelectList(const SelectBlock& block, const Schema& scope, Grouping* grouping,
                                 OutputList& outputs)
        {
            if (block.star)
            {
                for (const Column& column : scope.columns())
                {
                    if (grouping != nullptr)
                    {
                        requireGroupingColumn(*grouping, column, *block.star);
                    }
                    addOutput(outputs, refOf(column), "", *block.star);
                }
                return;
            }
            for (const SelectItem& item : block.items)
            {
                const std::string alias = item.alias ? item.alias->text : "";
                if (const ColumnName* name = std::get_if<ColumnName>(&item.expression))
                {
                    const Column column = resolveColumn(*name, scope);
                    if (grouping != nullptr)
                    {
                        requireGroupingColumn(*grouping, column, name->pos);
                    }
                    // D4: the alias is only an ORDER BY name; the project
                    // writes the source column.
                    addOutput(outputs, refOf(column), alias, name->pos);
                    continue;
                }
                const AggregateCall& call = std::get<AggregateCall>(item.expression);
                Type type = Type::Int;
                Aggregate aggregate = checkAggregate(call, scope, type);
                aggregate.alias = alias.empty() ? aggregateOutputName(aggregate) : alias;
                addOutput(outputs, ColumnRef{std::nullopt, aggregate.alias}, aggregate.alias, call.pos);
                refuseSameAggregate(*grouping, aggregate, call.pos);
                grouping->aggregates.push_back(aggregate);
            }
        }

        bool contains(const std::vector<ColumnRef>& columns, const ColumnRef& column)
        {
            for (const ColumnRef& candidate : columns)
            {
                if (candidate == column)
                {
                    return true;
                }
            }
            return false;
        }

        // ORDER BY (§M3.3): first an output name of the select list, then a
        // FROM column. A FROM column that is not selected is allowed (case 9)
        // and goes into `extra`, so the project below the sort can carry it
        // (D3), with D3's two side rules.
        std::vector<SortKey> translateOrderBy(const SelectBlock& block, const Schema& scope, const Grouping* grouping,
                                              const OutputList& outputs, std::vector<ColumnRef>& extra)
        {
            std::vector<SortKey> keys;
            for (const SortItem& item : block.orderBy)
            {
                std::optional<ColumnRef> target;
                if (!item.column.qualifier)
                {
                    for (std::size_t i = 0; i < outputs.sortNames.size(); ++i)
                    {
                        if (outputs.sortNames[i] == item.column.name)
                        {
                            target = outputs.columns[i];
                            break;
                        }
                    }
                }
                if (!target)
                {
                    const Column column = resolveColumn(item.column, scope);
                    const ColumnRef ref = refOf(column);
                    if (!contains(outputs.columns, ref))
                    {
                        // D3 rule 1: one name can come with many ages, so the
                        // order of distinct rows would be undefined.
                        if (block.distinct)
                        {
                            failAt(ErrorCategory::Name,
                                   "ORDER BY column '" + column.qualifiedName() +
                                       "' must appear in the select list when DISTINCT is used.",
                                   item.column.pos);
                        }
                        // D3 rule 2: above a group_by only keys and aggregates
                        // exist.
                        if (grouping != nullptr)
                        {
                            bool isKey = false;
                            for (const Column& key : grouping->keys)
                            {
                                isKey = isKey || key.qualifiedName() == column.qualifiedName();
                            }
                            if (!isKey)
                            {
                                failAt(ErrorCategory::Aggregate,
                                       "ORDER BY column '" + column.qualifiedName() +
                                           "' must be a grouping column or an aggregate output.",
                                       item.column.pos);
                            }
                        }
                        if (!contains(extra, ref))
                        {
                            extra.push_back(ref);
                        }
                    }
                    target = ref;
                }
                keys.push_back(SortKey{*target, item.ascending});
            }
            return keys;
        }

        // §M3.5 steps 2-7 on top of the FROM plan already in `plan`, whose
        // schema is `scope`. Not recursive: its locals are gone before any
        // other block is translated.
        void translateClausesAfterFrom(const SelectBlock& block, const Schema& scope, PlanNode& plan)
        {
            // 2. WHERE: one select node with the whole condition.
            if (block.where)
            {
                Condition where;
                convertCondition(*block.where, scope, nullptr, where);
                plan = PlanNode::select(std::move(where), std::move(plan));
            }

            // 3. GROUP BY keys and the select list (checked here, built at 5).
            const bool aggregating = isAggregating(block);
            Grouping grouping;
            for (const ColumnName& key : block.groupBy)
            {
                grouping.keys.push_back(resolveColumn(key, scope));
            }
            OutputList outputs;
            translateSelectList(block, scope, aggregating ? &grouping : nullptr, outputs);

            if (aggregating)
            {
                // 4. HAVING is converted first, because it may add aggregates
                //    the select list does not have; then the group_by holds
                //    every aggregate, and HAVING's select sits above it.
                Condition having;
                if (block.having)
                {
                    convertCondition(*block.having, scope, &grouping, having);
                }
                std::vector<ColumnRef> keys;
                for (const Column& key : grouping.keys)
                {
                    keys.push_back(refOf(key));
                }
                plan = PlanNode::groupBy(std::move(keys), grouping.aggregates, std::move(plan));
                if (block.having)
                {
                    plan = PlanNode::select(std::move(having), std::move(plan));
                }
            }

            // 6. ORDER BY keys (resolved before the project is built, because
            //    a non-selected key widens it).
            std::vector<ColumnRef> extra;
            const std::vector<SortKey> sortKeys =
                translateOrderBy(block, scope, aggregating ? &grouping : nullptr, outputs, extra);

            // 5. SELECT: the project (keeps duplicates, §M3.4), then distinct
            //    above it only for SELECT DISTINCT, and never above a GROUP BY
            //    (§M3.4: "do not emit it").
            std::vector<ColumnRef> projected = outputs.columns;
            projected.insert(projected.end(), extra.begin(), extra.end());
            plan = PlanNode::project(std::move(projected), std::move(plan));
            if (block.distinct && !aggregating)
            {
                plan = PlanNode::distinct(std::move(plan));
            }

            // 6. The sort; with non-selected keys, a final project drops them
            //    again (D3): project → sort → project(widened).
            if (!sortKeys.empty())
            {
                plan = PlanNode::sort(sortKeys, std::move(plan));
            }
            if (!extra.empty())
            {
                plan = PlanNode::project(outputs.columns, std::move(plan));
            }

            // 7. LIMIT on top.
            if (block.limit)
            {
                plan = PlanNode::limit(*block.limit, std::move(plan));
            }
        }

        // One select block, §M3.5 steps 1-7, into `out`.
        void translateSelectBlock(const SelectBlock& block, const TableLookup& lookup, PlanNode& out)
        {
            Schema scope;
            translateFrom(block.from, lookup, out, scope);
            translateClausesAfterFrom(block, scope, out);
        }

        // The plan op of a set operator. UNION and UNION ALL are different
        // nodes (§M3.4): `union` removes duplicates, `union_all` keeps them;
        // INTERSECT and EXCEPT are set operations only.
        PlanOp setOpPlanOp(SetOpKind kind)
        {
            switch (kind)
            {
                case SetOpKind::Union:
                    return PlanOp::Union;
                case SetOpKind::UnionAll:
                    return PlanOp::UnionAll;
                case SetOpKind::Intersect:
                    return PlanOp::Intersect;
                case SetOpKind::Except:
                    return PlanOp::Except;
            }
            throw std::logic_error("setOpPlanOp: unknown SetOpKind value");
        }

        // §3.2: "Set operators require the same column count and the same
        // type in each position." Common's checkUnionCompatible has that rule
        // (Error(Schema), required case 14); the error gets the position of
        // the operator keyword. Not recursive.
        void checkSetOperation(const SetOperation& operation, const TableLookup& lookup, const PlanNode& out)
        {
            const Schema left = outputSchema(out.children[0], lookup);
            const Schema right = outputSchema(out.children[1], lookup);
            try
            {
                checkUnionCompatible(left, right, opName(out.op));
            }
            catch (const Error& error)
            {
                failAt(error.category(), error.message(), operation.pos);
            }
        }

        // A whole query into `out`: a select block, or a set operator over
        // two queries (§M3.5 step 8). The parser already folded chains to the
        // left, so `A UNION B EXCEPT C` arrives as Except(Union(A, B), C) and
        // this walk keeps that shape: left associative, one level (§M3.2).
        // Both sides are built straight into the node's two child slots.
        void translateInto(const QueryExpr& query, const TableLookup& lookup, PlanNode& out)
        {
            if (const SelectBlock* block = std::get_if<SelectBlock>(&query.node))
            {
                translateSelectBlock(*block, lookup, out);
                return;
            }
            const SetOperation& operation = std::get<SetOperation>(query.node);
            out.op = setOpPlanOp(operation.kind);
            out.children.resize(2);
            translateInto(operation.operands[0], lookup, out.children[0]);
            translateInto(operation.operands[1], lookup, out.children[1]);
            checkSetOperation(operation, lookup, out);
        }

        // --- Plan height (common's depth rule, limits.h) ----------------
        //
        // The parser limits SQL nesting, but one SQL level can become several
        // plan levels: each FROM subquery adds a rename and a project, and a
        // WHERE adds a select. So 500 nested subqueries already give a plan
        // of 1001 levels, which common's JSON reader (opt, exec) refuses and
        // which its recursive code is not built for (at 1000 nested
        // subqueries the JSON writer overflowed the stack). The finished
        // plan is therefore measured the way the reader will count it.

        // Levels the reader counts while it parses a condition's plan text
        // (common's toString, Plan style): each and/or/not is one level, and
        // each parenthesis the text needs is one more. The printer writes
        // `not (…)` unless the child is a comparison, `(a or b) and c` for a
        // lower-precedence left child, and `a and (b and c)` for a right child
        // of lower or equal precedence (Or < And < Not < Compare).
        int conditionLevels(const Condition& condition)
        {
            switch (condition.kind)
            {
                case ConditionKind::Compare:
                    return 0;
                case ConditionKind::Not:
                {
                    const Condition& child = condition.children[0];
                    const int parenthesis = child.kind == ConditionKind::Compare ? 0 : 1;
                    return 1 + parenthesis + conditionLevels(child);
                }
                case ConditionKind::And:
                case ConditionKind::Or:
                {
                    const Condition& left = condition.children[0];
                    const Condition& right = condition.children[1];
                    // A child gets parentheses when it is an Or under an
                    // And; a right child also when it is the same connective.
                    const bool leftParenthesis =
                        condition.kind == ConditionKind::And && left.kind == ConditionKind::Or;
                    const bool rightParenthesis =
                        right.kind == ConditionKind::Or
                        || (condition.kind == ConditionKind::And && right.kind == ConditionKind::And);
                    const int leftLevels = (leftParenthesis ? 1 : 0) + conditionLevels(left);
                    const int rightLevels = (rightParenthesis ? 1 : 0) + conditionLevels(right);
                    return 1 + std::max(leftLevels, rightLevels);
                }
            }
            return 0;
        }

        // Levels along the deepest path below and including `node`: a node
        // with inputs is one level (a scan is a leaf, zero), and its
        // condition continues from the node's own level, as in the reader.
        int planLevels(const PlanNode& node)
        {
            if (node.children.empty())
            {
                return 0;
            }
            int below = node.condition ? conditionLevels(*node.condition) : 0;
            for (const PlanNode& child : node.children)
            {
                below = std::max(below, planLevels(child));
            }
            return 1 + below;
        }
    } // namespace

    PlanNode translateQuery(const QueryExpr& query, const TableLookup& lookup)
    {
        PlanNode plan;
        translateInto(query, lookup, plan);
        // Every plan sql hands out must fit common's limit, so it can be
        // written, read back and run. The error points at the query's first
        // SELECT, or at its last set operator keyword.
        if (planLevels(plan) > kMaxNestingDepth)
        {
            const SelectBlock* block = std::get_if<SelectBlock>(&query.node);
            const SourcePos pos = block ? block->pos : std::get<SetOperation>(query.node).pos;
            failAt(ErrorCategory::Syntax,
                   "Query nested too deep: its plan has more than " + std::to_string(kMaxNestingDepth) + " levels.",
                   pos);
        }
        return plan;
    }

} // namespace db::sql
