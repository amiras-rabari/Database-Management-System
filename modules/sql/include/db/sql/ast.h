#pragma once

/// @file
/// The abstract syntax tree (AST): what the parser builds from SQL text.
///
/// The tree says only what the user wrote. It holds no catalog facts and no
/// resolved names (that is the semantic pass, chunks 07-10), so `sql parse`
/// works without a catalog (§M3.7). Every node a later error can point at
/// carries a SourcePos, so errors can say `at line L, column C` (§M3.6).
///
/// All types are plain values. Children live in std::vector members, so
/// copying a tree copies all of it, and `==` compares whole trees (positions
/// included). A struct may hold a std::vector of its own type, because a
/// vector does not need the full type where it is declared (C++17).
///
/// The kinds of functions, operators and condition nodes reuse common's
/// enums (AggregateFunction, CompareOp, ConditionKind), so the translator
/// copies them into the plan without any mapping.

#include <db/common/condition.h>
#include <db/common/plan.h>
#include <db/common/value.h>
#include <db/sql/source_pos.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace db::sql
{

    /// A name as written, with its position: a table name, an alias, or a
    /// column name in CREATE TABLE.
    struct Name
    {
        std::string text;
        SourcePos pos;

        friend bool operator==(const Name&, const Name&) = default;
    };

    /// A column reference as written: `name` or `qualifier.name`
    /// (`column ::= ident | ident "." ident`). `pos` is where it starts.
    struct ColumnName
    {
        std::optional<std::string> qualifier;
        std::string name;
        SourcePos pos;

        friend bool operator==(const ColumnName&, const ColumnName&) = default;
    };

    /// An integer or string literal: an INT or VARCHAR Value.
    struct Literal
    {
        Value value;
        SourcePos pos;

        friend bool operator==(const Literal&, const Literal&) = default;
    };

    /// An aggregate call such as `COUNT(*)` or `AVG(salary)`.
    /// An empty `argument` means `*`. The grammar allows `*` for every
    /// function; the checker (chunk 08) rejects it for all but COUNT.
    struct AggregateCall
    {
        AggregateFunction function = AggregateFunction::Count;
        std::optional<ColumnName> argument;
        /// Position of the function name.
        SourcePos pos;

        friend bool operator==(const AggregateCall&, const AggregateCall&) = default;
    };

    /// One side of a comparison. An aggregate call is allowed here because
    /// HAVING compares aggregates (example 5.4: `HAVING COUNT(*) > 5`); the
    /// checker rejects one in WHERE or ON with an `aggregate` error (case 7).
    using Operand = std::variant<ColumnName, Literal, AggregateCall>;

    /// A condition tree (WHERE, ON, HAVING), shaped like common's Condition
    /// but with positions and aggregate operands.
    /// - Compare: `left op right`; `children` is empty.
    /// - And / Or: two or more `children`; `left`, `right`, `op` unused.
    /// - Not: exactly one child.
    struct SqlCondition
    {
        ConditionKind kind = ConditionKind::Compare;
        CompareOp op = CompareOp::Equal;
        Operand left;
        Operand right;
        std::vector<SqlCondition> children;
        /// Compare: position of the left operand. Not: position of NOT.
        /// And / Or: position of the first child.
        SourcePos pos;

        friend bool operator==(const SqlCondition&, const SqlCondition&) = default;
    };

    /// One item of the select list: a column or an aggregate, with an
    /// optional `AS` alias (`select_item ::= ( column | aggregate ) [ AS ident ]`).
    struct SelectItem
    {
        std::variant<ColumnName, AggregateCall> expression;
        std::optional<Name> alias;

        friend bool operator==(const SelectItem&, const SelectItem&) = default;
    };

    struct QueryExpr;

    /// One entry of a FROM clause: a base table or a parenthesised subquery,
    /// with an optional alias. The parser makes sure a subquery has an alias
    /// (§M3.3).
    struct TableRef
    {
        /// The table name. Unused (empty) for a subquery.
        Name table;
        /// Empty for a base table; exactly one query for a subquery.
        std::vector<QueryExpr> subquery;
        std::optional<Name> alias;

        friend bool operator==(const TableRef&, const TableRef&) = default;
    };

    /// A FROM clause: either a comma list `A, B, C` or a JOIN chain
    /// `A JOIN B ON c1 JOIN C ON c2` (the grammar never mixes them).
    /// Every JOIN needs an ON, so `onConditions` is empty for a comma list
    /// (or a single table) and holds tables.size() - 1 conditions for a JOIN
    /// chain: onConditions[k] joins tables[k + 1] to everything before it.
    struct FromClause
    {
        std::vector<TableRef> tables;
        std::vector<SqlCondition> onConditions;

        friend bool operator==(const FromClause&, const FromClause&) = default;
    };

    /// One ORDER BY item: a column and its direction (ASC is the default).
    struct SortItem
    {
        ColumnName column;
        bool ascending = true;

        friend bool operator==(const SortItem&, const SortItem&) = default;
    };

    /// One `SELECT ... FROM ...` block with all its optional clauses.
    /// GROUP BY and ORDER BY lists are never empty when written (the grammar
    /// needs at least one item), so an empty vector means "not written".
    struct SelectBlock
    {
        /// Position of the SELECT keyword.
        SourcePos pos;
        bool distinct = false;
        /// Set for `SELECT *`: the position of the `*`. Then `items` is empty.
        std::optional<SourcePos> star;
        std::vector<SelectItem> items;
        FromClause from;
        std::optional<SqlCondition> where;
        std::vector<ColumnName> groupBy;
        std::optional<SqlCondition> having;
        std::vector<SortItem> orderBy;
        std::optional<int32_t> limit;

        friend bool operator==(const SelectBlock&, const SelectBlock&) = default;
    };

    /// The four set operators. UNION and UNION ALL are different kinds,
    /// because they become different plan nodes (§M3.4, example 5.7).
    enum class SetOpKind
    {
        Union,
        UnionAll,
        Intersect,
        Except
    };

    /// `left kind right`. The parser folds a chain to the left, so
    /// `A UNION B EXCEPT C` is Except(Union(A, B), C): the tree shape itself
    /// is the left associativity (§M3.2).
    struct SetOperation
    {
        SetOpKind kind = SetOpKind::Union;
        /// Exactly two queries: left, then right.
        std::vector<QueryExpr> operands;
        /// Position of the operator keyword (UNION, INTERSECT, EXCEPT); the
        /// schema error for incompatible inputs points here (case 14).
        SourcePos pos;

        friend bool operator==(const SetOperation&, const SetOperation&) = default;
    };

    /// A query: one select block, or a set operation over two queries.
    struct QueryExpr
    {
        std::variant<SelectBlock, SetOperation> node;

        friend bool operator==(const QueryExpr&, const QueryExpr&) = default;
    };

    /// One column of CREATE TABLE: `name INT` or `name VARCHAR(n)`.
    /// The length is kept as written; common's Schema rejects n outside
    /// 1..255 later (chunk 11), because that is a schema rule, not syntax.
    struct ColumnDef
    {
        Name name;
        Type type = Type::Int;
        /// VARCHAR only: n as written. 0 for INT.
        int32_t varcharLength = 0;

        friend bool operator==(const ColumnDef&, const ColumnDef&) = default;
    };

    /// `CREATE TABLE name ( coldef { , coldef } )`.
    struct CreateTableStmt
    {
        Name table;
        std::vector<ColumnDef> columns;

        friend bool operator==(const CreateTableStmt&, const CreateTableStmt&) = default;
    };

    /// `INSERT INTO name VALUES ( literal { , literal } )`: one row.
    struct InsertStmt
    {
        Name table;
        std::vector<Literal> values;

        friend bool operator==(const InsertStmt&, const InsertStmt&) = default;
    };

    /// One statement: `statement ::= create | insert | query`.
    using Statement = std::variant<CreateTableStmt, InsertStmt, QueryExpr>;

} // namespace db::sql
