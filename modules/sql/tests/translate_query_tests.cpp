// FROM subqueries and set operators (chunk 10, §M3.3-§M3.5 step 8):
// examples 5.6 and 5.7, required cases 4, 8 and 14, associativity, and the
// deepest queries the parser accepts.

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

TEST_CASE("subquery: example 5.6, translated on its own, then renamed")
{
    CHECK(planText("SELECT t.did, t.n FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t WHERE t.n > 3;") ==
          "project[t.did, t.n](select[t.n > 3](rename[t](project[Emp.did, n](group_by[Emp.did; count(*) as n]"
          "(scan(Emp))))))");
}

TEST_CASE("subquery: the outer query sees only the alias and the subquery's outputs")
{
    const std::string sql = "SELECT t.name FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t";
    checkError(sql, ErrorCategory::Name, "Table 't' has no column 'name'.", 8);
    // The inner table name is hidden too.
    checkError("SELECT Emp.did FROM (SELECT did FROM Emp) AS t", ErrorCategory::Name,
               "Unknown table or alias 'Emp' in 'Emp.did'.", 8);
    // A bare name finds the subquery's column.
    CHECK(planText("SELECT n FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t") ==
          "project[t.n](rename[t](project[Emp.did, n](group_by[Emp.did; count(*) as n](scan(Emp)))))");
}

TEST_CASE("subquery: required case 8, a subquery joined to a base table")
{
    CHECK(planText("SELECT t.did, d.title FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t "
                   "JOIN Dept d ON t.did = d.did") ==
          "project[t.did, d.title](join[t.did = d.did](rename[t](project[Emp.did, n](group_by[Emp.did; "
          "count(*) as n](scan(Emp)))), rename[d](scan(Dept))))");
}

TEST_CASE("subquery: two outputs with one name after the rename (decision (a))")
{
    const std::string sql = "SELECT t.did FROM (SELECT e.did, d.did FROM Emp e, Dept d) AS t";
    checkError(sql, ErrorCategory::Schema, "Duplicate column 't.did'.", columnOf(sql, "t"));
}

TEST_CASE("set operators: example 5.7 / required case 4, UNION ALL and UNION differ in one node")
{
    const db::PlanNode all = compile("SELECT city FROM Emp UNION ALL SELECT city FROM Dept;");
    const db::PlanNode distinct = compile("SELECT city FROM Emp UNION SELECT city FROM Dept;");
    CHECK(db::toString(all) == "union_all(project[Emp.city](scan(Emp)), project[Dept.city](scan(Dept)))");
    CHECK(db::toString(distinct) == "union(project[Emp.city](scan(Emp)), project[Dept.city](scan(Dept)))");
    CHECK(all.op == db::PlanOp::UnionAll);
    CHECK(distinct.op == db::PlanOp::Union);
    CHECK(all.children == distinct.children);
}

TEST_CASE("set operators: INTERSECT and EXCEPT")
{
    CHECK(planText("SELECT did FROM Emp INTERSECT SELECT did FROM Dept") ==
          "intersect(project[Emp.did](scan(Emp)), project[Dept.did](scan(Dept)))");
    CHECK(planText("SELECT did FROM Emp EXCEPT SELECT did FROM Dept") ==
          "except(project[Emp.did](scan(Emp)), project[Dept.did](scan(Dept)))");
}

TEST_CASE("set operators: required case 14, inputs must match in count and types")
{
    const std::string types = "SELECT name FROM Emp UNION SELECT did FROM Dept";
    checkError(types, ErrorCategory::Schema,
               "Relation schemas are not union-compatible for 'union': column 1 is VARCHAR on the left but INT on "
               "the right.",
               columnOf(types, "UNION"));
    const std::string count = "SELECT name, age FROM Emp EXCEPT SELECT title FROM Dept";
    checkError(count, ErrorCategory::Schema,
               "Relation schemas are not union-compatible for 'except': column counts differ (2 vs 1).",
               columnOf(count, "EXCEPT"));
}

TEST_CASE("set operators: left associative at one level (§M3.2)")
{
    CHECK(planText("SELECT city FROM Emp UNION SELECT city FROM Dept EXCEPT SELECT city FROM Emp") ==
          "except(union(project[Emp.city](scan(Emp)), project[Dept.city](scan(Dept))), "
          "project[Emp.city](scan(Emp)))");
    // Parentheses keep the grouping the user wrote.
    CHECK(planText("SELECT city FROM Emp UNION (SELECT city FROM Dept EXCEPT SELECT city FROM Emp)") ==
          "union(project[Emp.city](scan(Emp)), except(project[Dept.city](scan(Dept)), "
          "project[Emp.city](scan(Emp))))");
}

TEST_CASE("set operators: a union in FROM takes the left side's column names")
{
    CHECK(planText("SELECT x.city FROM (SELECT city FROM Emp UNION SELECT city FROM Dept) AS x") ==
          "project[x.city](rename[x](union(project[Emp.city](scan(Emp)), project[Dept.city](scan(Dept)))))");
}

TEST_CASE("set operators: each block keeps its own ORDER BY and LIMIT (grammar D10)")
{
    CHECK(planText("SELECT city FROM Emp UNION SELECT city FROM Dept ORDER BY city LIMIT 3") ==
          "union(project[Emp.city](scan(Emp)), limit[3](sort[Dept.city asc](project[Dept.city](scan(Dept)))))");
}

// The parser allows 1000 SQL levels, but common's limit is on plan levels
// (limits.h): the plan must fit too, so it can be written, read back and
// run. Deeper input is an error, never a crash. compile() also writes each
// plan to JSON and reads it back.
TEST_CASE("depth: plans stop at common's 1000 levels, with an error, never a crash")
{
    const std::string tooDeep = "Query nested too deep: its plan has more than 1000 levels.";

    // n nested FROM subqueries: project + rename per level, plus the inner
    // project, so 2n + 1 plan levels. 499 fit (999), 500 do not (1001).
    const auto nested = [](int levels)
    {
        std::string sql = "SELECT lid FROM Loc";
        for (int i = 0; i < levels; ++i)
        {
            sql = "SELECT lid FROM (" + sql + ") AS t";
        }
        return sql;
    };
    CHECK(compile(nested(499)).op == db::PlanOp::Project);
    db::sql::test::checkError(nested(500), db::ErrorCategory::Syntax, tooDeep, 1);
    // The deepest SQL the parser accepts: an error, not a stack overflow.
    db::sql::test::checkError(nested(db::kMaxNestingDepth), db::ErrorCategory::Syntax, tooDeep, 1);

    // n UNION ALLs fold left: n union_all levels over a project, n + 1.
    // 999 fit (1000), 1000 do not. The error points at the last UNION.
    const auto chain = [](int operators)
    {
        std::string sql = "SELECT lid FROM Loc";
        for (int i = 0; i < operators; ++i)
        {
            sql += " UNION ALL SELECT lid FROM Loc";
        }
        return sql;
    };
    CHECK(compile(chain(999)).op == db::PlanOp::UnionAll);
    const std::string longest = chain(db::kMaxNestingDepth);
    db::sql::test::checkError(longest, db::ErrorCategory::Syntax, tooDeep,
                              db::sql::test::columnOf(longest, "UNION"));
}
