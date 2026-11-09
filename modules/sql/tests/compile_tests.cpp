// compileSql (chunk 11): CREATE TABLE and INSERT become checked values for
// the shell to execute; queries become logical plans. Every test uses the
// in-memory test catalog (Emp, Dept, Proj, Loc).

#include "test_catalog.h"
#include "translate_test_helpers.h"

#include <db/sql/compile.h>

#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/common/value.h>

#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

using db::ErrorCategory;
using db::Type;
using db::Value;
using db::sql::CompiledStatement;
using db::sql::CreateTablePlan;
using db::sql::InsertPlan;
using db::sql::compileSql;
using db::sql::test::columnOf;
using db::sql::test::testCatalog;

namespace
{
    // The one statement `sql` compiles to.
    CompiledStatement compileOne(const std::string& sql)
    {
        INFO("sql: ", sql);
        std::vector<CompiledStatement> statements = compileSql(sql, testCatalog());
        REQUIRE(statements.size() == 1);
        return statements[0];
    }

    // Checks the error compileSql gives: category, message and position
    // (line 1, `column`).
    void checkCompileError(const std::string& sql, ErrorCategory category, const std::string& message, int column)
    {
        INFO("sql: ", sql);
        try
        {
            compileSql(sql, testCatalog());
            FAIL("compiled without an error");
        }
        catch (const db::Error& error)
        {
            CHECK(error.category() == category);
            CHECK(error.message() == message);
            CHECK(error.line() == 1);
            CHECK(error.column() == column);
        }
    }
} // namespace

TEST_CASE("compile: CREATE TABLE gives the table name and a schema qualified by it")
{
    const CompiledStatement statement = compileOne("CREATE TABLE T (a INT, b VARCHAR(20))");
    REQUIRE(std::holds_alternative<CreateTablePlan>(statement));
    const CreateTablePlan& create = std::get<CreateTablePlan>(statement);
    CHECK(create.table == "T");
    REQUIRE(create.schema.size() == 2);
    const db::Column& a = create.schema.column(0);
    const db::Column& b = create.schema.column(1);
    CHECK(a.qualifiedName() == "T.a");
    CHECK(a.type == Type::Int);
    CHECK(b.qualifiedName() == "T.b");
    CHECK(b.type == Type::Varchar);
    CHECK(b.varcharLength == 20);
}

TEST_CASE("compile: CREATE TABLE errors come from common's Schema, at the table name")
{
    const std::string tooShort = "CREATE TABLE T (a VARCHAR(0))";
    checkCompileError(tooShort, ErrorCategory::Schema, "Column 'T.a' is VARCHAR(0); n must be 1 to 255.",
                      columnOf(tooShort, "T "));
    const std::string tooLong = "CREATE TABLE T (a INT, b VARCHAR(256))";
    checkCompileError(tooLong, ErrorCategory::Schema, "Column 'T.b' is VARCHAR(256); n must be 1 to 255.",
                      columnOf(tooLong, "T "));
    const std::string twice = "CREATE TABLE T (a INT, a INT)";
    checkCompileError(twice, ErrorCategory::Schema, "Duplicate column 'T.a'.", columnOf(twice, "T "));

    // 255 is the largest legal length.
    const CompiledStatement statement = compileOne("CREATE TABLE T (a VARCHAR(255))");
    CHECK(std::get<CreateTablePlan>(statement).schema.column(0).varcharLength == 255);
}

TEST_CASE("compile: INSERT gives one value per column, in schema order")
{
    const CompiledStatement statement = compileOne("INSERT INTO Dept VALUES (1, 'it''s', 500, 'Ottawa')");
    REQUIRE(std::holds_alternative<InsertPlan>(statement));
    const InsertPlan& insert = std::get<InsertPlan>(statement);
    CHECK(insert.table == "Dept");
    CHECK(insert.tuple == db::Tuple{Value::ofInt(1), Value::ofVarchar("it's"), Value::ofInt(500),
                                    Value::ofVarchar("Ottawa")});

    // Signed integers, the whole int32 range.
    const CompiledStatement edges = compileOne("INSERT INTO Dept VALUES (-5, 'x', 2147483647, 'y')");
    CHECK(std::get<InsertPlan>(edges).tuple[0] == Value::ofInt(-5));
    CHECK(std::get<InsertPlan>(edges).tuple[2] == Value::ofInt(2147483647));
}

TEST_CASE("compile: an INSERT string is never an INT, even when it looks like one")
{
    const std::string sql = "INSERT INTO Dept VALUES ('1', 'x', 500, 'y')";
    checkCompileError(sql, ErrorCategory::Type, "Value '1' is VARCHAR, but column 'Dept.did' is INT.",
                      columnOf(sql, "'1'"));
    const std::string number = "INSERT INTO Dept VALUES (1, 2, 500, 'y')";
    checkCompileError(number, ErrorCategory::Type, "Value '2' is INT, but column 'Dept.title' is VARCHAR.",
                      columnOf(number, "2,"));
}

TEST_CASE("compile: INSERT with the wrong number of values, into an unknown table, or a string too long")
{
    const std::string few = "INSERT INTO Dept VALUES (1, 'x', 500)";
    checkCompileError(few, ErrorCategory::Schema, "INSERT has 3 values but table 'Dept' has 4 columns.",
                      columnOf(few, "Dept"));
    const std::string unknown = "INSERT INTO Nope VALUES (1)";
    checkCompileError(unknown, ErrorCategory::Name, "Unknown table 'Nope'.", columnOf(unknown, "Nope"));

    // 21 characters for title VARCHAR(20): common's parseValue rule.
    const std::string longText = "INSERT INTO Dept VALUES (1, 'abcdefghijklmnopqrstu', 500, 'y')";
    checkCompileError(longText, ErrorCategory::Schema,
                      "'abcdefghijklmnopqrstu' is longer than VARCHAR(20) for column Dept.title.",
                      columnOf(longText, "'abc"));
}

TEST_CASE("compile: a script gives its statements in order; the first error stops everything")
{
    const std::vector<CompiledStatement> statements =
        compileSql("INSERT INTO Dept VALUES (1, 'x', 500, 'y'); SELECT name, age FROM Emp WHERE age > 30;"
                   "SELECT city FROM Emp UNION ALL SELECT city FROM Dept;",
                   testCatalog());
    REQUIRE(statements.size() == 3);
    CHECK(std::holds_alternative<InsertPlan>(statements[0]));
    REQUIRE(std::holds_alternative<db::PlanNode>(statements[1]));
    CHECK(db::toString(std::get<db::PlanNode>(statements[1])) ==
          db::sql::test::planText("SELECT name, age FROM Emp WHERE age > 30"));
    REQUIRE(std::holds_alternative<db::PlanNode>(statements[2]));
    CHECK(std::get<db::PlanNode>(statements[2]).op == db::PlanOp::UnionAll);

    // The second statement is wrong: its error, and nothing is returned.
    const std::string bad = "SELECT name FROM Emp; SELECT nope FROM Emp; SELECT age FROM Emp";
    checkCompileError(bad, ErrorCategory::Name, "Unknown attribute 'nope'.", columnOf(bad, "nope"));

    // No overlay: a table created earlier in the same script is still
    // unknown, because compileSql only consults the lookup.
    const std::string created = "CREATE TABLE T (a INT); SELECT a FROM T";
    checkCompileError(created, ErrorCategory::Name, "Unknown table 'T'.", columnOf(created, "T"));

    CHECK(compileSql("-- only a comment\n;", testCatalog()).empty());
}
