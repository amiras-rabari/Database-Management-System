// Type checks, aggregate rules and ORDER BY resolution (chunk 08, §M3.3
// §3.1 / §3.2, D3, D4): required cases 6, 7, 9 and 10, and the errors of
// each rule, with their positions.

#include "translate_test_helpers.h"

#include <db/common/error.h>
#include <db/common/limits.h>
#include <db/common/plan.h>

#include <doctest/doctest.h>

#include <string>

using db::ErrorCategory;
using db::sql::test::checkError;
using db::sql::test::columnOf;
using db::sql::test::compile;
using db::sql::test::planText;

TEST_CASE("checks: required case 10, comparisons need the same type")
{
    const std::string sql = "SELECT name FROM Emp WHERE age > '30'";
    checkError(sql, ErrorCategory::Type, "Incompatible comparison operand types: INT and VARCHAR.", columnOf(sql, "age"));
    // Equality too (§3.2 "including equality").
    const std::string equal = "SELECT name FROM Emp WHERE name = 5";
    checkError(equal, ErrorCategory::Type, "Incompatible comparison operand types: VARCHAR and INT.", columnOf(equal, "name"));
    // Two literals of one type, and VARCHARs of different lengths, are fine.
    CHECK(planText("SELECT name FROM Emp WHERE 1 = 1") == "project[Emp.name](select[1 = 1](scan(Emp)))");
    CHECK(planText("SELECT e.name FROM Emp e, Dept d WHERE e.name = d.title") ==
          "project[e.name](select[e.name = d.title](cross(rename[e](scan(Emp)), rename[d](scan(Dept)))))");
    // In an ON as well.
    const std::string on = "SELECT * FROM Emp e JOIN Dept d ON e.did = d.title";
    checkError(on, ErrorCategory::Type, "Incompatible comparison operand types: INT and VARCHAR.", columnOf(on, "e.did"));
}

TEST_CASE("checks: required case 6, a non-grouped column in an aggregating select list")
{
    const std::string sql = "SELECT name, COUNT(*) FROM Emp GROUP BY did";
    checkError(sql, ErrorCategory::Aggregate,
               "Column 'Emp.name' must appear in GROUP BY or be used in an aggregate.", 8);
    // SELECT * expands first, so its first non-grouped column fails.
    checkError("SELECT * FROM Emp GROUP BY did", ErrorCategory::Aggregate,
               "Column 'Emp.eid' must appear in GROUP BY or be used in an aggregate.", 8);
    // Grouping columns match after resolution: did and Emp.did are one column.
    CHECK(planText("SELECT Emp.did FROM Emp GROUP BY did") ==
          "project[Emp.did](group_by[Emp.did; ](scan(Emp)))");
}

TEST_CASE("checks: required case 7, an aggregate in WHERE or ON")
{
    const std::string sql = "SELECT name FROM Emp WHERE COUNT(*) > 1";
    checkError(sql, ErrorCategory::Aggregate,
               "Aggregate COUNT(*) is not allowed in WHERE or ON: they run before grouping.", columnOf(sql, "COUNT"));
    const std::string on = "SELECT * FROM Emp e JOIN Dept d ON MAX(e.did) = d.did";
    checkError(on, ErrorCategory::Aggregate,
               "Aggregate MAX(e.did) is not allowed in WHERE or ON: they run before grouping.", columnOf(on, "MAX"));
}

TEST_CASE("checks: HAVING may use grouping columns and aggregates only")
{
    // REAL (AVG) against INT is a legal comparison (DECISIONS 2026-11-07).
    CHECK(planText("SELECT did FROM Emp GROUP BY did HAVING AVG(salary) > 50000") ==
          "project[Emp.did](select[avg_Emp_salary > 50000](group_by[Emp.did; avg(Emp.salary) as "
          "avg_Emp_salary](scan(Emp))))");
    const std::string sql = "SELECT did FROM Emp GROUP BY did HAVING age > 3";
    checkError(sql, ErrorCategory::Aggregate,
               "Column 'Emp.age' must appear in GROUP BY or be used in an aggregate.", columnOf(sql, "age"));
    // An output alias is not visible in HAVING (settled in the chunk 08 guide).
    const std::string alias = "SELECT did, COUNT(*) AS n FROM Emp GROUP BY did HAVING n > 5";
    checkError(alias, ErrorCategory::Name, "Unknown attribute 'n'.", columnOf(alias, "n"));
    // An aggregate's result type is checked like a column: COUNT is INT.
    const std::string typed = "SELECT did FROM Emp GROUP BY did HAVING COUNT(*) = 'x'";
    checkError(typed, ErrorCategory::Type, "Incompatible comparison operand types: INT and VARCHAR.", columnOf(typed, "COUNT"));
}

TEST_CASE("checks: aggregate arguments (§3.2)")
{
    const std::string avg = "SELECT did, AVG(name) FROM Emp GROUP BY did";
    checkError(avg, ErrorCategory::Type, "Aggregate 'avg' needs an INT column, but 'Emp.name' is VARCHAR(20).", columnOf(avg, "AVG"));
    checkError("SELECT SUM(*) FROM Emp", ErrorCategory::Type, "Only COUNT may use '*', not SUM(*).", 8);
    // MIN and MAX take any type; COUNT takes any column.
    CHECK(planText("SELECT MAX(name), COUNT(city) FROM Emp") ==
          "project[max_Emp_name, count_Emp_city](group_by[; max(Emp.name) as max_Emp_name, count(Emp.city) as "
          "count_Emp_city](scan(Emp)))");
}

TEST_CASE("checks: an aggregate alone makes a query aggregating (case 13)")
{
    CHECK(planText("SELECT COUNT(*) FROM Emp") == "project[count_star](group_by[; count(*) as count_star](scan(Emp)))");
}

TEST_CASE("checks: duplicate output columns are refused, as in exec (decision (a))")
{
    checkError("SELECT name, name FROM Emp", ErrorCategory::Schema, "Duplicate output column 'Emp.name'.", 14);
    checkError("SELECT COUNT(*), COUNT(*) FROM Emp", ErrorCategory::Schema, "Duplicate output column 'count_star'.",
               18);
    checkError("SELECT COUNT(*) AS n, SUM(age) AS n FROM Emp", ErrorCategory::Schema,
               "Duplicate output column 'n'.", 23);
    // Two aliases of one aggregate are two output columns.
    CHECK(planText("SELECT COUNT(*) AS a, COUNT(*) AS b FROM Emp") ==
          "project[a, b](group_by[; count(*) as a, count(*) as b](scan(Emp)))");
}

TEST_CASE("checks: ORDER BY looks at output aliases first, then FROM (§M3.3, D3, D4)")
{
    // An aggregate alias stays bare (example 5.4).
    CHECK(planText("SELECT did, COUNT(*) AS n FROM Emp GROUP BY did ORDER BY n") ==
          "sort[n asc](project[Emp.did, n](group_by[Emp.did; count(*) as n](scan(Emp))))");
    // A generated name works too.
    CHECK(planText("SELECT did, COUNT(*) FROM Emp GROUP BY did ORDER BY count_star DESC") ==
          "sort[count_star desc](project[Emp.did, count_star](group_by[Emp.did; count(*) as count_star](scan(Emp))))");
    // D4: a plain column's alias sorts by the source column.
    CHECK(planText("SELECT name AS n FROM Emp ORDER BY n") == "sort[Emp.name asc](project[Emp.name](scan(Emp)))");
    // D3 rule 1: DISTINCT with a non-selected key.
    const std::string distinct = "SELECT DISTINCT name FROM Emp ORDER BY age";
    checkError(distinct, ErrorCategory::Name,
               "ORDER BY column 'Emp.age' must appear in the select list when DISTINCT is used.",
               columnOf(distinct, "age"));
    // D3 rule 2: an aggregating query sorts by keys and aggregate outputs only.
    const std::string grouped = "SELECT did, COUNT(*) FROM Emp GROUP BY did ORDER BY age";
    checkError(grouped, ErrorCategory::Aggregate,
               "ORDER BY column 'Emp.age' must be a grouping column or an aggregate output.", columnOf(grouped, "age"));
}

TEST_CASE("checks: a condition 1000 levels deep converts without a crash")
{
    std::string sql = "SELECT a FROM Loc WHERE ";
    for (int i = 0; i < db::kMaxNestingDepth; ++i)
    {
        sql += "NOT ";
    }
    sql += "lid = 1";
    // The column `a` does not exist, but WHERE is converted first, so the
    // whole deep condition is walked before the select list fails.
    const db::Error error = db::sql::test::compileError(sql);
    CHECK(error.message() == "Unknown attribute 'a'.");
}

// The plan of `SELECT lid FROM Loc WHERE NOT ... NOT lid = 1` with k NOTs, as
// common's JSON reader counts it: project 1 + select 1 + the condition text
// `not (not (... not lid = 1))`, where every NOT but the last also needs a
// parenthesis: 2k - 1. So 2k + 1 levels: 499 NOTs fit (999), 500 do not.
TEST_CASE("checks: a NOT chain whose plan would pass common's limit is refused")
{
    const auto query = [](int nots)
    {
        std::string sql = "SELECT lid FROM Loc WHERE ";
        for (int i = 0; i < nots; ++i)
        {
            sql += "NOT ";
        }
        return sql + "lid = 1";
    };
    // compile() also writes the plan to JSON and reads it back.
    CHECK(compile(query(499)).op == db::PlanOp::Project);
    db::sql::test::checkError(query(500), db::ErrorCategory::Syntax,
                              "Query nested too deep: its plan has more than 1000 levels.", 1);
    db::sql::test::checkError(query(db::kMaxNestingDepth), db::ErrorCategory::Syntax,
                              "Query nested too deep: its plan has more than 1000 levels.", 1);

    // k right-nested ANDs, `lid = 1 AND (lid = 1 AND (... ))`: common prints a
    // right child of the same connective in parentheses (the parser folds
    // left), so each AND but the last costs 2 levels: again 2k + 1 in all.
    const auto rightNested = [](int ands)
    {
        std::string sql = "lid = 1";
        for (int i = 0; i < ands; ++i)
        {
            sql = "lid = 1 AND (" + sql + ")";
        }
        return "SELECT lid FROM Loc WHERE " + sql;
    };
    CHECK(compile(rightNested(499)).op == db::PlanOp::Project);
    db::sql::test::checkError(rightNested(500), db::ErrorCategory::Syntax,
                              "Query nested too deep: its plan has more than 1000 levels.", 1);
}
