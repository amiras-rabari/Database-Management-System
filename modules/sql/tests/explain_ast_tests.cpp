// explainAst (chunk 05): the indented parse tree that `sql parse --explain`
// prints. Each test parses real SQL and compares the whole text with a
// golden string, so a change in any line, indent or keyword shows up.

#include <db/sql/explain_ast.h>
#include <db/sql/parser.h>

#include <db/common/limits.h>

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace
{
    // The tree of a text that holds exactly one statement.
    std::string explain(const std::string& sql)
    {
        const std::vector<db::sql::Statement> statements = db::sql::parseSqlScript(sql);
        REQUIRE(statements.size() == 1);
        return db::sql::explainAst(statements[0]);
    }
} // namespace

TEST_CASE("explainAst: example 5.7, UNION ALL and UNION differ in exactly the root line")
{
    const std::string unionAll = explain("SELECT city FROM Emp UNION ALL SELECT city FROM Dept;");
    const std::string unionDistinct = explain("SELECT city FROM Emp UNION SELECT city FROM Dept;");
    CHECK(unionAll == R"(union_all
  select_block
    item: city
    from
      table: Emp
  select_block
    item: city
    from
      table: Dept
)");
    CHECK(unionDistinct == R"(union
  select_block
    item: city
    from
      table: Emp
  select_block
    item: city
    from
      table: Dept
)");
    // Only the first line differs: the rest is the same text.
    const std::string restAll = unionAll.substr(unionAll.find('\n'));
    const std::string restDistinct = unionDistinct.substr(unionDistinct.find('\n'));
    CHECK(restAll == restDistinct);
}

TEST_CASE("explainAst: NOT > AND > OR is visible as parentheses")
{
    CHECK(explain("SELECT a FROM R WHERE a = 1 OR b = 2 AND NOT c = 3") == R"(select_block
  item: a
  from
    table: R
  where: a = 1 OR (b = 2 AND (NOT c = 3))
)");
    // A chain folds to the left; explicit parentheses keep the user's grouping.
    CHECK(explain("SELECT a FROM R WHERE a = 1 OR b = 2 OR c = 3") == R"(select_block
  item: a
  from
    table: R
  where: (a = 1 OR b = 2) OR c = 3
)");
    CHECK(explain("SELECT a FROM R WHERE NOT (a = 1 OR b = 2) AND NOT NOT c = 3") == R"(select_block
  item: a
  from
    table: R
  where: (NOT (a = 1 OR b = 2)) AND (NOT (NOT c = 3))
)");
}

TEST_CASE("explainAst: example 5.1")
{
    CHECK(explain("SELECT name, age FROM Emp WHERE age > 30;") == R"(select_block
  item: name
  item: age
  from
    table: Emp
  where: age > 30
)");
}

TEST_CASE("explainAst: example 5.4, every clause of a select block")
{
    CHECK(explain("SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal FROM Emp WHERE age > 25 "
                  "GROUP BY did HAVING COUNT(*) > 5 ORDER BY n DESC, did LIMIT 10;") == R"(select_block
  item: did
  item: COUNT(*) AS n
  item: AVG(salary) AS avg_sal
  from
    table: Emp
  where: age > 25
  group by: did
  having: COUNT(*) > 5
  order by: n DESC, did ASC
  limit: 10
)");
}

TEST_CASE("explainAst: JOIN chain (5.2) and comma list (5.3)")
{
    CHECK(explain("SELECT e.name, d.title FROM Emp e JOIN Dept d ON e.did = d.did "
                  "JOIN Loc AS l ON d.lid = l.lid WHERE d.budget > 1000") == R"(select_block
  item: e.name
  item: d.title
  from: join chain
    table: Emp AS e
    table: Dept AS d
      on: e.did = d.did
    table: Loc AS l
      on: d.lid = l.lid
  where: d.budget > 1000
)");
    CHECK(explain("SELECT * FROM Emp e, Dept d WHERE e.did = d.did") == R"(select_block
  select: *
  from: comma list
    table: Emp AS e
    table: Dept AS d
  where: e.did = d.did
)");
}

TEST_CASE("explainAst: subquery in FROM (5.6)")
{
    CHECK(explain("SELECT t.did, t.n FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t "
                  "WHERE t.n > 3") == R"(select_block
  item: t.did
  item: t.n
  from
    subquery AS t
      select_block
        item: did
        item: COUNT(*) AS n
        from
          table: Emp
        group by: did
  where: t.n > 3
)");
}

TEST_CASE("explainAst: DISTINCT, literals, set operators folding left")
{
    CHECK(explain("select distinct name from Emp where name = 'O''Brien' or age >= -5") == R"(select_block distinct
  item: name
  from
    table: Emp
  where: name = 'O''Brien' OR age >= -5
)");
    // A UNION B EXCEPT C is (A UNION B) EXCEPT C; parentheses override it.
    CHECK(explain("SELECT a FROM A UNION SELECT a FROM B EXCEPT SELECT a FROM C") == R"(except
  union
    select_block
      item: a
      from
        table: A
    select_block
      item: a
      from
        table: B
  select_block
    item: a
    from
      table: C
)");
    CHECK(explain("SELECT a FROM A INTERSECT (SELECT a FROM B UNION ALL SELECT a FROM C)") == R"(intersect
  select_block
    item: a
    from
      table: A
  union_all
    select_block
      item: a
      from
        table: B
    select_block
      item: a
      from
        table: C
)");
}

TEST_CASE("explainAst: CREATE TABLE and INSERT")
{
    CHECK(explain("CREATE TABLE Emp (eid INT, name VARCHAR(20))") == R"(create_table Emp
  column: eid INT
  column: name VARCHAR(20)
)");
    CHECK(explain("INSERT INTO Emp VALUES (1, 'Bob', -7)") == R"(insert Emp
  values: 1, 'Bob', -7
)");
}

TEST_CASE("explainAst: the deepest trees the parser accepts print without a crash")
{
    // 1000 NOTs: the condition printer recurses once per NOT.
    std::string notChain = "SELECT a FROM R WHERE ";
    for (int i = 0; i < db::kMaxNestingDepth; ++i)
    {
        notChain += "NOT ";
    }
    notChain += "a = 1";
    const std::string notTree = explain(notChain);
    CHECK(notTree.find("where: NOT (NOT (NOT ") != std::string::npos);

    // 1000 nested FROM subqueries: the query printer recurses once per level.
    std::string nested = "SELECT a FROM R";
    for (int i = 0; i < db::kMaxNestingDepth; ++i)
    {
        nested = "SELECT a FROM (" + nested + ") AS t";
    }
    const std::string nestedTree = explain(nested);
    CHECK(nestedTree.find("table: R") != std::string::npos);
}
