// Translation of one select block in canonical order (chunk 09, §M3.5):
// examples 5.1-5.5 and required cases 1, 5, 9, 13 and 15. Examples 5.1 and
// 5.2 are compared with the spec's JSON byte for byte; the rest with
// common's one-line plan text.

#include "translate_test_helpers.h"

#include <db/common/plan.h>
#include <db/common/plan_json.h>

#include <doctest/doctest.h>

#include <string>

using db::sql::test::compile;
using db::sql::test::planText;

TEST_CASE("translate: example 5.1 is the spec's JSON")
{
    CHECK(db::toJsonText(compile("SELECT name, age FROM Emp WHERE age > 30;"), -1) ==
          R"({"op":"project","columns":["Emp.name","Emp.age"],)"
          R"("child":{"op":"select","condition":"Emp.age > 30","child":{"op":"scan","table":"Emp"}}})");
}

TEST_CASE("translate: example 5.2 is the spec's JSON, WHERE stays above the join")
{
    CHECK(db::toJsonText(compile("SELECT e.name, d.title FROM Emp e JOIN Dept d ON e.did = d.did "
                                 "WHERE d.budget > 1000;"),
                         -1) ==
          R"({"op":"project","columns":["e.name","d.title"],)"
          R"("child":{"op":"select","condition":"d.budget > 1000",)"
          R"("child":{"op":"join","condition":"e.did = d.did",)"
          R"("left":{"op":"rename","to":"e","child":{"op":"scan","table":"Emp"}},)"
          R"("right":{"op":"rename","to":"d","child":{"op":"scan","table":"Dept"}}}}})");
}

TEST_CASE("translate: example 5.3, a comma gives cross with the predicate above")
{
    CHECK(planText("SELECT * FROM Emp e, Dept d WHERE e.did = d.did;") ==
          "project[e.eid, e.name, e.age, e.did, e.salary, e.mgr, e.city, d.did, d.title, d.budget, d.city]"
          "(select[e.did = d.did](cross(rename[e](scan(Emp)), rename[d](scan(Dept)))))");
}

TEST_CASE("translate: example 5.4 (required case 5), WHERE below and HAVING above the grouping")
{
    CHECK(planText("SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal FROM Emp WHERE age > 25 GROUP BY did "
                   "HAVING COUNT(*) > 5 ORDER BY n DESC LIMIT 10;") ==
          "limit[10](sort[n desc](project[Emp.did, n, avg_sal](select[n > 5](group_by[Emp.did; count(*) as n, "
          "avg(Emp.salary) as avg_sal](select[Emp.age > 25](scan(Emp)))))))");
}

TEST_CASE("translate: example 5.5 (required case 1), a self join with two renames")
{
    CHECK(planText("SELECT e.name, m.name FROM Emp e JOIN Emp m ON e.mgr = m.eid;") ==
          "project[e.name, m.name](join[e.mgr = m.eid](rename[e](scan(Emp)), rename[m](scan(Emp))))");
}

TEST_CASE("translate: required case 13, COUNT(*) without GROUP BY")
{
    // The plan half. The "one row out, even when Emp is empty" half is
    // exec's test "group_by: no keys + empty input gives exactly one row
    // with COUNT 0 (sql case 13)" in modules/exec/tests/group_by_tests.cpp.
    CHECK(planText("SELECT COUNT(*) FROM Emp") == "project[count_star](group_by[; count(*) as count_star](scan(Emp)))");
}

TEST_CASE("translate: required case 15, four tables with commas and with JOIN ON")
{
    const std::string commas = planText("SELECT e.name FROM Emp e, Dept d, Proj p, Loc l "
                                        "WHERE e.did = d.did AND d.did = p.did AND p.lid = l.lid");
    const std::string joins = planText("SELECT e.name FROM Emp e JOIN Dept d ON e.did = d.did "
                                       "JOIN Proj p ON d.did = p.did JOIN Loc l ON p.lid = l.lid");
    CHECK(commas == "project[e.name](select[e.did = d.did and d.did = p.did and p.lid = l.lid]("
                    "cross(cross(cross(rename[e](scan(Emp)), rename[d](scan(Dept))), rename[p](scan(Proj))), "
                    "rename[l](scan(Loc)))))");
    CHECK(joins == "project[e.name](join[p.lid = l.lid](join[d.did = p.did](join[e.did = d.did]("
                   "rename[e](scan(Emp)), rename[d](scan(Dept))), rename[p](scan(Proj))), rename[l](scan(Loc))))");
}

TEST_CASE("translate: required case 9, ORDER BY a column that is not selected (D3)")
{
    CHECK(planText("SELECT name FROM Emp ORDER BY age") ==
          "project[Emp.name](sort[Emp.age asc](project[Emp.name, Emp.age](scan(Emp))))");
    CHECK(planText("SELECT name FROM Emp ORDER BY age LIMIT 5") ==
          "limit[5](project[Emp.name](sort[Emp.age asc](project[Emp.name, Emp.age](scan(Emp)))))");
    // A grouping key that is not selected.
    CHECK(planText("SELECT COUNT(*) FROM Emp GROUP BY did ORDER BY did") ==
          "project[count_star](sort[Emp.did asc](project[count_star, Emp.did](group_by[Emp.did; count(*) as "
          "count_star](scan(Emp)))))");
    // A selected key needs no extra project.
    CHECK(planText("SELECT name, age FROM Emp ORDER BY age DESC, name") ==
          "sort[Emp.age desc, Emp.name asc](project[Emp.name, Emp.age](scan(Emp)))");
}

TEST_CASE("translate: distinct only for SELECT DISTINCT, above the project (§M3.4)")
{
    CHECK(planText("SELECT DISTINCT name FROM Emp") == "distinct(project[Emp.name](scan(Emp)))");
    CHECK(planText("SELECT name FROM Emp") == "project[Emp.name](scan(Emp))");
    CHECK(planText("SELECT DISTINCT name FROM Emp ORDER BY name") ==
          "sort[Emp.name asc](distinct(project[Emp.name](scan(Emp))))");
    // Never above a GROUP BY: it already gives one row per group.
    CHECK(planText("SELECT DISTINCT did FROM Emp GROUP BY did") == "project[Emp.did](group_by[Emp.did; ](scan(Emp)))");
}

TEST_CASE("translate: aggregate names (D2) and reuse of one call")
{
    // No AS: common's generated name.
    CHECK(planText("SELECT did, AVG(salary) FROM Emp GROUP BY did") ==
          "project[Emp.did, avg_Emp_salary](group_by[Emp.did; avg(Emp.salary) as avg_Emp_salary](scan(Emp)))");
    // A HAVING-only aggregate gets its own entry; the project leaves it out.
    CHECK(planText("SELECT did FROM Emp GROUP BY did HAVING SUM(salary) > 100") ==
          "project[Emp.did](select[sum_Emp_salary > 100](group_by[Emp.did; sum(Emp.salary) as "
          "sum_Emp_salary](scan(Emp))))");
    // The same call in HAVING reuses the select item's alias (5.4: n > 5).
    CHECK(planText("SELECT did, MAX(age) AS oldest FROM Emp GROUP BY did HAVING MAX(age) > 60 AND did > 1") ==
          "project[Emp.did, oldest](select[oldest > 60 and Emp.did > 1](group_by[Emp.did; max(Emp.age) as "
          "oldest](scan(Emp))))");
}

TEST_CASE("translate: the condition keeps its shape (no predicate moved or split)")
{
    CHECK(planText("SELECT name FROM Emp WHERE NOT (age > 3 OR city = 'x') AND did = 1") ==
          "project[Emp.name](select[not (Emp.age > 3 or Emp.city = 'x') and Emp.did = 1](scan(Emp)))");
}
