// Tests for the AST value types (CHUNK_02 section 4). Most AST testing
// happens through the parser (chunks 03-04); these check that the trees are
// plain values: copies compare equal, and a deep tree is safe.

#include <db/sql/ast.h>

#include <db/common/condition.h>
#include <db/common/value.h>
#include <db/sql/source_pos.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using db::CompareOp;
using db::ConditionKind;
using db::Value;
using db::sql::ColumnName;
using db::sql::Literal;
using db::sql::Name;
using db::sql::QueryExpr;
using db::sql::SelectBlock;
using db::sql::SelectItem;
using db::sql::SetOperation;
using db::sql::SetOpKind;
using db::sql::SqlCondition;
using db::sql::TableRef;

namespace
{

    // A comparison `column op literal`.
    SqlCondition compare(ColumnName column, CompareOp op, Literal literal, db::sql::SourcePos pos)
    {
        SqlCondition condition;
        condition.kind = ConditionKind::Compare;
        condition.op = op;
        condition.left = std::move(column);
        condition.right = std::move(literal);
        condition.pos = pos;
        return condition;
    }

    // `SELECT a FROM <table>` at the start of line `line`:
    // `a` is at column 8 and the table name at column 15.
    QueryExpr selectAFrom(const std::string& table, int32_t line)
    {
        SelectBlock block;
        block.pos = {line, 1};
        block.items.push_back(SelectItem{ColumnName{{}, "a", {line, 8}}, {}});
        block.from.tables.push_back(TableRef{Name{table, {line, 15}}, {}, {}});
        return QueryExpr{std::move(block)};
    }

    // A set operation `left kind right`.
    QueryExpr setOp(SetOpKind kind, QueryExpr left, QueryExpr right, db::sql::SourcePos pos)
    {
        SetOperation operation;
        operation.kind = kind;
        operation.operands.push_back(std::move(left));
        operation.operands.push_back(std::move(right));
        operation.pos = pos;
        return QueryExpr{std::move(operation)};
    }

} // namespace

TEST_CASE("example 5.1 built by hand: copies are equal, a changed literal is not")
{
    // SELECT name, age FROM Emp WHERE age > 30;
    // 123456789012345678901234567890123456789
    SelectBlock block;
    block.pos = {1, 1};
    block.items.push_back(SelectItem{ColumnName{{}, "name", {1, 8}}, {}});
    block.items.push_back(SelectItem{ColumnName{{}, "age", {1, 14}}, {}});
    block.from.tables.push_back(TableRef{Name{"Emp", {1, 23}}, {}, {}});
    block.where = compare(ColumnName{{}, "age", {1, 33}}, CompareOp::Greater, Literal{Value::ofInt(30), {1, 39}}, {1, 33});
    const QueryExpr query{block};

    const QueryExpr copy = query;
    CHECK(copy == query);

    SelectBlock changed = block;
    std::get<Literal>(changed.where->right).value = Value::ofInt(31);
    CHECK(QueryExpr{changed} != query);

    // Positions take part in ==, so a position bug shows up in tests.
    SelectBlock moved = block;
    moved.items[0] = SelectItem{ColumnName{{}, "name", {1, 9}}, {}};
    CHECK(QueryExpr{moved} != query);
}

TEST_CASE("A UNION B EXCEPT C is Except(Union(A, B), C), not Union(A, Except(B, C))")
{
    const QueryExpr a = selectAFrom("A", 1);
    const QueryExpr b = selectAFrom("B", 2);
    const QueryExpr c = selectAFrom("C", 3);

    const QueryExpr leftFolded =
        setOp(SetOpKind::Except, setOp(SetOpKind::Union, a, b, {1, 20}), c, {2, 20});
    const QueryExpr rightFolded =
        setOp(SetOpKind::Union, a, setOp(SetOpKind::Except, b, c, {2, 20}), {1, 20});
    CHECK(leftFolded != rightFolded);
    CHECK(leftFolded == QueryExpr{leftFolded});

    // UNION and UNION ALL are different kinds (§M3.4, example 5.7).
    CHECK(setOp(SetOpKind::Union, a, b, {1, 20}) != setOp(SetOpKind::UnionAll, a, b, {1, 20}));
}

TEST_CASE("a NOT chain of depth 1000 can be built, copied and destroyed")
{
    SqlCondition condition = compare(ColumnName{{}, "a", {1, 1}}, CompareOp::Equal, Literal{Value::ofInt(1), {1, 5}}, {1, 1});
    for (int depth = 0; depth < 1000; ++depth)
    {
        SqlCondition wrapper;
        wrapper.kind = ConditionKind::Not;
        wrapper.children.push_back(std::move(condition));
        condition = std::move(wrapper);
    }
    const SqlCondition copy = condition;
    CHECK(copy == condition);
}
