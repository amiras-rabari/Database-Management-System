// Name resolution (chunk 07, §M3.3): the scope built from FROM, qualified
// and unqualified columns, SELECT *, and the errors of required cases 2, 3,
// 11 and 12. Columns are checked through the plans they end up in.

#include "translate_test_helpers.h"

#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/plan_json.h>

#include <doctest/doctest.h>

#include <string>

using db::ErrorCategory;
using db::sql::test::checkError;
using db::sql::test::columnOf;
using db::sql::test::compile;
using db::sql::test::planText;

TEST_CASE("resolution: example 5.1, bare columns get the table name")
{
    CHECK(planText("SELECT name, age FROM Emp WHERE age > 30") ==
          "project[Emp.name, Emp.age](select[Emp.age > 30](scan(Emp)))");
}

TEST_CASE("resolution: example 5.2, aliases are the qualifiers")
{
    CHECK(planText("SELECT e.name, d.title FROM Emp e JOIN Dept d ON e.did = d.did WHERE d.budget > 1000") ==
          "project[e.name, d.title](select[d.budget > 1000](join[e.did = d.did](rename[e](scan(Emp)), "
          "rename[d](scan(Dept)))))");
    // A bare column unique in the scope also gets the alias: title is only in Dept.
    CHECK(planText("SELECT title FROM Emp e JOIN Dept d ON e.did = d.did") ==
          "project[d.title](join[e.did = d.did](rename[e](scan(Emp)), rename[d](scan(Dept))))");
}

TEST_CASE("resolution: required case 2, SELECT * over three tables sharing names")
{
    // did and city are in two tables each: no error, every column qualified,
    // FROM order then schema order.
    CHECK(planText("SELECT * FROM Emp e, Dept d, Loc l") ==
          "project[e.eid, e.name, e.age, e.did, e.salary, e.mgr, e.city, d.did, d.title, d.budget, d.city, "
          "l.lid, l.region](cross(cross(rename[e](scan(Emp)), rename[d](scan(Dept))), rename[l](scan(Loc))))");
}

TEST_CASE("resolution: required case 3, an unqualified column in two tables")
{
    const std::string sql = "SELECT did FROM Emp, Dept";
    checkError(sql, ErrorCategory::Ambiguity,
               "Ambiguous unqualified attribute 'did' (matches Emp.did, Dept.did). Use a relation qualifier.", 8);
}

TEST_CASE("resolution: required case 11, unknown table and unknown column read differently")
{
    checkError("SELECT a FROM Nope", ErrorCategory::Name, "Unknown table 'Nope'.", 15);
    checkError("SELECT nope FROM Emp", ErrorCategory::Name, "Unknown attribute 'nope'.", 8);
    // Identifiers are case-sensitive: emp is not Emp.
    checkError("SELECT name FROM emp", ErrorCategory::Name, "Unknown table 'emp'.", 18);
}

TEST_CASE("resolution: required case 12, the same name twice in FROM")
{
    const std::string sql = "SELECT name FROM Emp, Emp";
    checkError(sql, ErrorCategory::Name, "Table name or alias 'Emp' is used twice in FROM.", columnOf(sql, "Emp"));
    const std::string aliased = "SELECT e.name FROM Emp e, Emp e";
    checkError(aliased, ErrorCategory::Name, "Table name or alias 'e' is used twice in FROM.", columnOf(aliased, "e"));
    // Two aliases for one table are fine (example 5.5).
    CHECK(planText("SELECT e.name FROM Emp e, Emp m") ==
          "project[e.name](cross(rename[e](scan(Emp)), rename[m](scan(Emp))))");
}

TEST_CASE("resolution: a qualified column names the part that failed")
{
    // After FROM Emp e, only e is in scope: Emp is hidden by its alias.
    const std::string hidden = "SELECT Emp.name FROM Emp e";
    checkError(hidden, ErrorCategory::Name, "Unknown table or alias 'Emp' in 'Emp.name'.", 8);
    checkError("SELECT e.nope FROM Emp e", ErrorCategory::Name, "Table 'e' has no column 'nope'.", 8);
}

TEST_CASE("resolution: D5, an ON sees only the tables joined so far")
{
    const std::string sql = "SELECT * FROM Emp e JOIN Dept d ON e.did = p.did JOIN Proj p ON p.did = d.did";
    checkError(sql, ErrorCategory::Name, "Unknown table or alias 'p' in 'p.did'.",
               static_cast<int>(sql.find("p.did")) + 1);
    // In the second ON, all three are visible.
    CHECK(planText("SELECT e.name FROM Emp e JOIN Dept d ON e.did = d.did JOIN Proj p ON p.did = e.did") ==
          "project[e.name](join[p.did = e.did](join[e.did = d.did](rename[e](scan(Emp)), rename[d](scan(Dept))), "
          "rename[p](scan(Proj))))");
}

TEST_CASE("resolution: no unqualified scope column survives in the plan JSON")
{
    const std::string json =
        db::toJsonText(compile("SELECT name FROM Emp e WHERE age > 3 AND city = 'x' ORDER BY salary"), -1);
    CHECK(json.find("\"name\"") == std::string::npos);
    CHECK(json.find("e.name") != std::string::npos);
    CHECK(json.find("e.age > 3 and e.city = 'x'") != std::string::npos);
    CHECK(json.find("e.salary") != std::string::npos);
}
