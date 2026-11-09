// Tests for the statement and query parser (CHUNK_04 section 4): the spec's
// examples 5.1-5.7, set-operator associativity, CREATE / INSERT, scripts,
// and syntax errors with positions. Each test checks the fields that matter
// instead of building whole trees by hand.

#include <db/sql/parser.h>

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/value.h>
#include <db/sql/ast.h>
#include <db/sql/source_pos.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

using db::ConditionKind;
using db::Error;
using db::ErrorCategory;
using db::Type;
using db::Value;
using db::sql::AggregateCall;
using db::sql::ColumnName;
using db::sql::CreateTableStmt;
using db::sql::InsertStmt;
using db::sql::QueryExpr;
using db::sql::SelectBlock;
using db::sql::SetOperation;
using db::sql::SetOpKind;
using db::sql::SourcePos;
using db::sql::Statement;
using db::sql::parseSqlQuery;
using db::sql::parseSqlScript;

namespace
{

    // Both helpers return a copy: tests call them on the temporary that
    // parseSqlQuery returns, and a reference into it would dangle.
    SelectBlock blockOf(const QueryExpr& query)
    {
        REQUIRE(std::holds_alternative<SelectBlock>(query.node));
        return std::get<SelectBlock>(query.node);
    }

    SetOperation setOpOf(const QueryExpr& query)
    {
        REQUIRE(std::holds_alternative<SetOperation>(query.node));
        return std::get<SetOperation>(query.node);
    }

    // The table name of the single FROM entry of a block.
    std::string fromTableOf(const QueryExpr& query)
    {
        const SelectBlock& block = blockOf(query);
        REQUIRE(block.from.tables.size() == 1);
        return block.from.tables[0].table.text;
    }

    const ColumnName& columnItem(const SelectBlock& block, std::size_t i)
    {
        REQUIRE(i < block.items.size());
        REQUIRE(std::holds_alternative<ColumnName>(block.items[i].expression));
        return std::get<ColumnName>(block.items[i].expression);
    }

    // Parses text that must fail with a syntax error at line:column.
    void checkErrorAt(const std::string& text, int line, int column, const std::string& message)
    {
        try
        {
            parseSqlScript(text);
        }
        catch (const Error& error)
        {
            CHECK_MESSAGE(error.category() == ErrorCategory::Syntax, text);
            CHECK_MESSAGE(error.line() == line, text);
            CHECK_MESSAGE(error.column() == column, text);
            CHECK_MESSAGE(error.message() == message, text);
            return;
        }
        FAIL("expected a syntax error for: " << text);
    }

    bool accepts(const std::string& text)
    {
        try
        {
            parseSqlScript(text);
            return true;
        }
        catch (const Error&)
        {
            return false;
        }
    }

    std::string repeated(const std::string& piece, int times)
    {
        std::string text;
        for (int i = 0; i < times; ++i)
        {
            text += piece;
        }
        return text;
    }

} // namespace

TEST_CASE("example 5.1: plain filter and projection")
{
    const SelectBlock& block = blockOf(parseSqlQuery("SELECT name, age FROM Emp WHERE age > 30;"));
    CHECK(block.pos == SourcePos{1, 1});
    CHECK_FALSE(block.distinct);
    CHECK_FALSE(block.star.has_value());
    CHECK(columnItem(block, 0).name == "name");
    CHECK(columnItem(block, 1).name == "age");
    CHECK(block.items.size() == 2);
    REQUIRE(block.from.tables.size() == 1);
    CHECK(block.from.tables[0].table.text == "Emp");
    CHECK(block.from.tables[0].table.pos == SourcePos{1, 23});
    CHECK_FALSE(block.from.tables[0].alias.has_value());
    CHECK(block.from.onConditions.empty());
    REQUIRE(block.where.has_value());
    CHECK(block.where->kind == ConditionKind::Compare);
    CHECK(std::get<ColumnName>(block.where->left).name == "age");
    CHECK(block.groupBy.empty());
    CHECK_FALSE(block.having.has_value());
    CHECK(block.orderBy.empty());
    CHECK_FALSE(block.limit.has_value());
}

TEST_CASE("example 5.2: JOIN chain with aliases written without AS")
{
    const SelectBlock& block = blockOf(parseSqlQuery("SELECT e.name, d.title\n"
                                                     "  FROM Emp e JOIN Dept d ON e.did = d.did\n"
                                                     " WHERE d.budget > 1000;"));
    CHECK(columnItem(block, 0).qualifier == "e");
    CHECK(columnItem(block, 1).name == "title");
    REQUIRE(block.from.tables.size() == 2);
    CHECK(block.from.tables[0].table.text == "Emp");
    CHECK(block.from.tables[0].alias->text == "e");
    CHECK(block.from.tables[1].table.text == "Dept");
    CHECK(block.from.tables[1].alias->text == "d");
    CHECK(block.from.tables[1].alias->pos == SourcePos{2, 24});
    REQUIRE(block.from.onConditions.size() == 1);
    CHECK(std::get<ColumnName>(block.from.onConditions[0].left).qualifier == "e");
    REQUIRE(block.where.has_value());
    CHECK(block.where->pos == SourcePos{3, 8});
}

TEST_CASE("example 5.3: SELECT * with a comma FROM; AS is optional on a table alias")
{
    const SelectBlock& block = blockOf(parseSqlQuery("SELECT * FROM Emp e, Dept AS d WHERE e.did = d.did;"));
    CHECK(block.star == SourcePos{1, 8});
    CHECK(block.items.empty());
    REQUIRE(block.from.tables.size() == 2);
    CHECK(block.from.tables[0].alias->text == "e");
    CHECK(block.from.tables[1].alias->text == "d");
    CHECK(block.from.onConditions.empty());
}

TEST_CASE("example 5.4: GROUP BY, HAVING, ORDER BY ... DESC, LIMIT, aggregate aliases")
{
    const SelectBlock& block = blockOf(parseSqlQuery("SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal\n"
                                                     "  FROM Emp\n"
                                                     " WHERE age > 25\n"
                                                     " GROUP BY did\n"
                                                     "HAVING COUNT(*) > 5\n"
                                                     " ORDER BY n DESC\n"
                                                     " LIMIT 10;"));
    REQUIRE(block.items.size() == 3);
    CHECK(columnItem(block, 0).name == "did");
    const AggregateCall& count = std::get<AggregateCall>(block.items[1].expression);
    CHECK(count.function == db::AggregateFunction::Count);
    CHECK_FALSE(count.argument.has_value());
    CHECK(block.items[1].alias->text == "n");
    const AggregateCall& avg = std::get<AggregateCall>(block.items[2].expression);
    CHECK(avg.function == db::AggregateFunction::Avg);
    CHECK(avg.argument->name == "salary");
    CHECK(block.items[2].alias->text == "avg_sal");
    REQUIRE(block.where.has_value());
    REQUIRE(block.groupBy.size() == 1);
    CHECK(block.groupBy[0].name == "did");
    REQUIRE(block.having.has_value());
    CHECK(std::holds_alternative<AggregateCall>(block.having->left));
    CHECK(block.having->pos == SourcePos{5, 8});
    REQUIRE(block.orderBy.size() == 1);
    CHECK(block.orderBy[0].column.name == "n");
    CHECK_FALSE(block.orderBy[0].ascending);
    CHECK(block.limit == 10);
}

TEST_CASE("ORDER BY directions are per item; ASC is the default")
{
    const SelectBlock& block = blockOf(parseSqlQuery("SELECT a FROM R ORDER BY a, b DESC, c ASC LIMIT 0"));
    REQUIRE(block.orderBy.size() == 3);
    CHECK(block.orderBy[0].ascending);
    CHECK_FALSE(block.orderBy[1].ascending);
    CHECK(block.orderBy[2].ascending);
    CHECK(block.limit == 0);
}

TEST_CASE("example 5.6: a subquery in FROM with an alias")
{
    const SelectBlock& outer = blockOf(parseSqlQuery("SELECT t.did, t.n\n"
                                                     "  FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t\n"
                                                     " WHERE t.n > 3;"));
    REQUIRE(outer.from.tables.size() == 1);
    const auto& ref = outer.from.tables[0];
    REQUIRE(ref.subquery.size() == 1);
    CHECK(ref.alias->text == "t");
    const SelectBlock& inner = blockOf(ref.subquery[0]);
    CHECK(inner.pos == SourcePos{2, 9});
    CHECK(inner.groupBy.size() == 1);
    CHECK(inner.items[1].alias->text == "n");
}

TEST_CASE("example 5.7: UNION ALL and UNION are different kinds")
{
    const SetOperation& all = setOpOf(parseSqlQuery("SELECT city FROM Emp UNION ALL SELECT city FROM Dept"));
    CHECK(all.kind == SetOpKind::UnionAll);
    CHECK(all.pos == SourcePos{1, 22});
    const SetOperation& plain = setOpOf(parseSqlQuery("SELECT city FROM Emp UNION SELECT city FROM Dept"));
    CHECK(plain.kind == SetOpKind::Union);
    REQUIRE(all.operands.size() == 2);
    // Same left block. The right blocks differ only in position: in the
    // UNION ALL query they start 4 columns later ("ALL ").
    CHECK(all.operands[0] == plain.operands[0]);
    CHECK(fromTableOf(all.operands[1]) == "Dept");
    CHECK(fromTableOf(plain.operands[1]) == "Dept");
}

TEST_CASE("set operators are left associative at one level (§M3.2)")
{
    // A UNION B EXCEPT C  ->  Except(Union(A, B), C)
    const SetOperation& top = setOpOf(parseSqlQuery("SELECT a FROM A UNION SELECT a FROM B EXCEPT SELECT a FROM C"));
    CHECK(top.kind == SetOpKind::Except);
    CHECK(fromTableOf(top.operands[1]) == "C");
    const SetOperation& left = setOpOf(top.operands[0]);
    CHECK(left.kind == SetOpKind::Union);
    CHECK(fromTableOf(left.operands[0]) == "A");
    CHECK(fromTableOf(left.operands[1]) == "B");

    // A EXCEPT B UNION C  ->  Union(Except(A, B), C): no operator binds tighter.
    const SetOperation& top2 = setOpOf(parseSqlQuery("SELECT a FROM A EXCEPT SELECT a FROM B UNION SELECT a FROM C"));
    CHECK(top2.kind == SetOpKind::Union);
    CHECK(setOpOf(top2.operands[0]).kind == SetOpKind::Except);

    // INTERSECT is at the same level too.
    const SetOperation& top3 = setOpOf(parseSqlQuery("SELECT a FROM A UNION SELECT a FROM B INTERSECT SELECT a FROM C"));
    CHECK(top3.kind == SetOpKind::Intersect);
}

TEST_CASE("parentheses around a block are accepted and can regroup set operators")
{
    const SetOperation& top = setOpOf(parseSqlQuery("(SELECT a FROM R) UNION (SELECT a FROM S)"));
    CHECK(fromTableOf(top.operands[0]) == "R");
    CHECK(fromTableOf(top.operands[1]) == "S");

    // A UNION (B EXCEPT C)
    const SetOperation& grouped = setOpOf(parseSqlQuery("SELECT a FROM A UNION (SELECT a FROM B EXCEPT SELECT a FROM C)"));
    CHECK(grouped.kind == SetOpKind::Union);
    CHECK(setOpOf(grouped.operands[1]).kind == SetOpKind::Except);
}

TEST_CASE("CREATE TABLE and INSERT")
{
    std::vector<Statement> statements =
        parseSqlScript("CREATE TABLE Emp (eid INT, name VARCHAR(20));\n"
                       "INSERT INTO Emp VALUES (1, 'it''s');\n"
                       "INSERT INTO Emp VALUES (-5, 'x')");
    REQUIRE(statements.size() == 3);

    const CreateTableStmt& create = std::get<CreateTableStmt>(statements[0]);
    CHECK(create.table.text == "Emp");
    REQUIRE(create.columns.size() == 2);
    CHECK(create.columns[0].name.text == "eid");
    CHECK(create.columns[0].type == Type::Int);
    CHECK(create.columns[1].type == Type::Varchar);
    CHECK(create.columns[1].varcharLength == 20);

    const InsertStmt& insert = std::get<InsertStmt>(statements[1]);
    CHECK(insert.table.text == "Emp");
    REQUIRE(insert.values.size() == 2);
    CHECK(insert.values[0].value == Value::ofInt(1));
    CHECK(insert.values[1].value == Value::ofVarchar("it's"));
    CHECK(insert.values[1].pos == SourcePos{2, 28});
    CHECK(std::get<InsertStmt>(statements[2]).values[0].value == Value::ofInt(-5));

    // The VARCHAR range is a schema rule (chunk 11), not syntax.
    CHECK(accepts("CREATE TABLE T (a VARCHAR(0), b VARCHAR(-3), c VARCHAR(999))"));
}

TEST_CASE("scripts: several statements, optional final ';', empty statements skipped")
{
    CHECK(parseSqlScript("SELECT a FROM R; SELECT b FROM S; SELECT c FROM T").size() == 3);
    CHECK(parseSqlScript("SELECT a FROM R;").size() == 1);
    CHECK(parseSqlScript(";; SELECT a FROM R ;; -- only a comment after\n").size() == 1);
    CHECK(parseSqlScript("").empty());
    CHECK(parseSqlScript("-- nothing but a comment").empty());
    checkErrorAt("SELECT a FROM R SELECT b FROM S", 1, 17, "expected ';' or end of input but found 'SELECT'");
}

TEST_CASE("parseSqlQuery takes exactly one query")
{
    CHECK(fromTableOf(parseSqlQuery("SELECT a FROM R")) == "R");
    CHECK_THROWS_AS(parseSqlQuery("SELECT a FROM R; SELECT a FROM S"), Error);
    CHECK_THROWS_AS(parseSqlQuery("CREATE TABLE T (a INT)"), Error);
}

TEST_CASE("syntax errors at the unexpected token")
{
    checkErrorAt("SELECT FROM R", 1, 8, "expected a column, an aggregate or '*' but found 'FROM'");
    checkErrorAt("SELECT a R", 1, 10, "expected FROM but found 'R'");
    checkErrorAt("SELECT a FROM R HAVING a > 1", 1, 17, "expected ';' or end of input but found 'HAVING'");
    checkErrorAt("SELECT a FROM (SELECT a FROM R)", 1, 32,
                 "expected AS and an alias (a subquery in FROM needs one) but found end of input");
    checkErrorAt("SELECT a FROM R, S JOIN T ON R.a = T.a", 1, 20, "expected ';' or end of input but found 'JOIN'");
    checkErrorAt("SELECT a FROM R JOIN S", 1, 23, "expected ON but found end of input");
    checkErrorAt("INSERT INTO R VALUES (1), (2)", 1, 25, "expected ';' or end of input but found ','");
    checkErrorAt("SELECT a FROM R LIMIT x", 1, 23, "expected a non-negative integer but found 'x'");
    checkErrorAt("SELECT a FROM R LIMIT -1", 1, 23, "expected a non-negative integer but found '-1'");
    checkErrorAt("SELECT a FROM R GROUP a", 1, 23, "expected BY but found 'a'");
    checkErrorAt("SELECT name n FROM R", 1, 13, "expected FROM but found 'n'");
    checkErrorAt("CREATE TABLE T (a TEXT)", 1, 19, "expected INT or VARCHAR but found 'TEXT'");
    checkErrorAt("INSERT INTO R VALUES (a)", 1, 23, "expected a literal (an integer or a string) but found 'a'");
    checkErrorAt("DELETE FROM R", 1, 1, "expected a statement (SELECT, CREATE TABLE or INSERT) but found 'DELETE'");
}

TEST_CASE("query nesting depth: 1000 levels work, 1001 is refused, never a crash")
{
    for (int depth : {1000, 1001})
    {
        const bool expected = depth <= 1000;
        const std::string parens = repeated("(", depth) + "SELECT a FROM R" + repeated(")", depth);
        CHECK_MESSAGE(accepts(parens) == expected, "parenthesised query depth " << depth);
        const std::string subqueries =
            repeated("SELECT a FROM (", depth) + "SELECT a FROM R" + repeated(") AS t", depth);
        CHECK_MESSAGE(accepts(subqueries) == expected, "FROM subquery depth " << depth);
    }
    CHECK_FALSE(accepts(repeated("(", 50000) + "SELECT a FROM R" + repeated(")", 50000)));
    CHECK_FALSE(accepts("SELECT a FROM R" + repeated(" UNION SELECT a FROM R", 2000)));
}
