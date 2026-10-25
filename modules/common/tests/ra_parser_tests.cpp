// The RA query parser: Bonus 1 queries, distinct, precedence and
// associativity of every binary operator pair, syntax errors with positions
// and the depth limit.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/plan.h"
#include "db/common/ra_parser.h"

namespace
{

    using db::ConditionStyle;
    using db::ErrorCategory;
    using db::PlanNode;

    // Runs `function` and returns the db::Error it threw, or nullopt if it
    // threw nothing.
    template <typename Function>
    std::optional<db::Error> errorFrom(Function function)
    {
        try
        {
            function();
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    std::string tree(const std::string& query)
    {
        return db::toString(db::parseRaQuery(query), ConditionStyle::Explicit);
    }

    std::optional<db::Error> parseError(const std::string& query)
    {
        return errorFrom([&] { db::parseRaQuery(query); });
    }

    void checkSyntaxError(const std::string& query, int line, int column)
    {
        INFO("query: ", query);
        const std::optional<db::Error> error = parseError(query);
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Syntax);
        CHECK(error->line() == line);
        CHECK(error->column() == column);
    }

    std::string repeat(const std::string& text, int count)
    {
        std::string out;
        for (int i = 0; i < count; ++i)
        {
            out += text;
        }
        return out;
    }

    struct TreeCase
    {
        const char* query;
        const char* expected;
    };

    void checkTrees(const std::vector<TreeCase>& cases)
    {
        for (const TreeCase& treeCase : cases)
        {
            INFO("query: ", treeCase.query);
            CHECK(tree(treeCase.query) == treeCase.expected);
        }
    }

} // namespace

// --- Bonus 1 queries --------------------------------------------------------

TEST_CASE("every Bonus 1 parse-only query parses to its expected tree")
{
    checkTrees({
        {"select[x1=3](R)", "select[(x1 = 3)](scan(R))"},
        {"select[ x1 = 3 ](R)", "select[(x1 = 3)](scan(R))"},
        {"select[Age>=30](R)", "select[(Age >= 30)](scan(R))"},
        {"select[Age>-30](R)", "select[(Age > -30)](scan(R))"},
        {"select[Name='Bob)'](R)", "select[(Name = 'Bob)')](scan(R))"},
        {"select[Name='a,b'](R)", "select[(Name = 'a,b')](scan(R))"},
        {"select[Name='O''Brien'](R)", "select[(Name = 'O''Brien')](scan(R))"},
        {"select[union=3](R)", "select[(union = 3)](scan(R))"},
        {"A union B minus C", "except(union(scan(A), scan(B)), scan(C))"},
        {"A minus B minus C", "except(except(scan(A), scan(B)), scan(C))"},
        {"select[not (a=1 and b=2) or c>3](R)", "select[((not ((a = 1) and (b = 2))) or (c > 3))](scan(R))"},
        {"select[a=1 and b=2 or c=3](R)", "select[(((a = 1) and (b = 2)) or (c = 3))](scan(R))"},
        {"project[Name](select[Age>30](select[DID='D1'](Employees)))",
         "project[Name](select[(Age > 30)](select[(DID = 'D1')](scan(Employees))))"},
        {"(A union B) minus (C intersect D)", "except(union(scan(A), scan(B)), intersect(scan(C), scan(D)))"},
    });
}

TEST_CASE("every Bonus 1 execution query parses to the expected tree")
{
    checkTrees({
        {"select[a=b](R)", "select[(a = b)](scan(R))"},
        {"Emp join[Emp.DID=Dept.DID] Dept", "join[(Emp.DID = Dept.DID)](scan(Emp), scan(Dept))"},
        {"rename[E2](Emp) join[Emp.MgrID=E2.EID] Emp", "join[(Emp.MgrID = E2.EID)](rename[E2](scan(Emp)), scan(Emp))"},
        {"R union S", "union(scan(R), scan(S))"},
        {"select[Age>'30'](Emp)", "select[(Age > '30')](scan(Emp))"},
        {"project[DID](Emp)", "project[DID](scan(Emp))"},
        {"project[Name, Name](Emp)", "project[Name, Name](scan(Emp))"},
        {"select[Age>999](Emp)", "select[(Age > 999)](scan(Emp))"},
    });
}

TEST_CASE("the parser builds the same tree as the named constructors")
{
    const PlanNode expected = PlanNode::join(db::parseCondition("Emp.MgrID = E2.EID"),
                                             PlanNode::rename("E2", PlanNode::scan("Emp")), PlanNode::scan("Emp"));
    CHECK(db::parseRaQuery("rename[E2](Emp) join[Emp.MgrID=E2.EID] Emp") == expected);
}

TEST_CASE("distinct(expr) is a unary operator")
{
    checkTrees({
        {"distinct(project[DID](Emp))", "distinct(project[DID](scan(Emp)))"},
        {"distinct(A union B)", "distinct(union(scan(A), scan(B)))"},
        {"distinct(A) times B", "cross(distinct(scan(A)), scan(B))"},
        {"A times distinct(B)", "cross(scan(A), distinct(scan(B)))"},
    });
}

TEST_CASE("project columns may be qualified and may be spelled like RA keywords")
{
    checkTrees({
        {"project[Emp.Name, Age](Emp)", "project[Emp.Name, Age](scan(Emp))"},
        {"project[union, R.select, distinct](R)", "project[union, R.select, distinct](scan(R))"},
    });
}

TEST_CASE("whitespace and comments between tokens are ignored")
{
    CHECK(tree("  A\n  union // the rest\n\tB  ") == "union(scan(A), scan(B))");
}

// --- Precedence and associativity -------------------------------------------

// Levels, loosest first: {union, minus} < intersect < {times, join} < unary.
// Within a level operators associate to the left.
TEST_CASE("union and minus share the loosest level and associate left")
{
    checkTrees({
        {"A union B union C", "union(union(scan(A), scan(B)), scan(C))"},
        {"A union B minus C", "except(union(scan(A), scan(B)), scan(C))"},
        {"A minus B union C", "union(except(scan(A), scan(B)), scan(C))"},
        {"A minus B minus C", "except(except(scan(A), scan(B)), scan(C))"},
    });
}

TEST_CASE("intersect binds tighter than union and minus")
{
    checkTrees({
        {"A union B intersect C", "union(scan(A), intersect(scan(B), scan(C)))"},
        {"A minus B intersect C", "except(scan(A), intersect(scan(B), scan(C)))"},
        {"A intersect B union C", "union(intersect(scan(A), scan(B)), scan(C))"},
        {"A intersect B minus C", "except(intersect(scan(A), scan(B)), scan(C))"},
        {"A intersect B intersect C", "intersect(intersect(scan(A), scan(B)), scan(C))"},
    });
}

TEST_CASE("times and join bind tighter than intersect, union and minus")
{
    checkTrees({
        {"A union B times C", "union(scan(A), cross(scan(B), scan(C)))"},
        {"A union B join[k=k] C", "union(scan(A), join[(k = k)](scan(B), scan(C)))"},
        {"A minus B times C", "except(scan(A), cross(scan(B), scan(C)))"},
        {"A minus B join[k=k] C", "except(scan(A), join[(k = k)](scan(B), scan(C)))"},
        {"A intersect B times C", "intersect(scan(A), cross(scan(B), scan(C)))"},
        {"A intersect B join[k=k] C", "intersect(scan(A), join[(k = k)](scan(B), scan(C)))"},
        {"A times B union C", "union(cross(scan(A), scan(B)), scan(C))"},
        {"A times B minus C", "except(cross(scan(A), scan(B)), scan(C))"},
        {"A times B intersect C", "intersect(cross(scan(A), scan(B)), scan(C))"},
        {"A join[k=k] B union C", "union(join[(k = k)](scan(A), scan(B)), scan(C))"},
        {"A join[k=k] B minus C", "except(join[(k = k)](scan(A), scan(B)), scan(C))"},
        {"A join[k=k] B intersect C", "intersect(join[(k = k)](scan(A), scan(B)), scan(C))"},
    });
}

TEST_CASE("times and join share a level and associate left")
{
    checkTrees({
        {"A times B times C", "cross(cross(scan(A), scan(B)), scan(C))"},
        {"A times B join[k=k] C", "join[(k = k)](cross(scan(A), scan(B)), scan(C))"},
        {"A join[k=k] B times C", "cross(join[(k = k)](scan(A), scan(B)), scan(C))"},
        {"A join[j=1] B join[k=2] C", "join[(k = 2)](join[(j = 1)](scan(A), scan(B)), scan(C))"},
    });
}

TEST_CASE("unary operators bind tightest and parentheses override precedence")
{
    checkTrees({
        {"select[a=1](A) union B", "union(select[(a = 1)](scan(A)), scan(B))"},
        {"A join[k=k] project[k](B)", "join[(k = k)](scan(A), project[k](scan(B)))"},
        {"rename[X](A) times B", "cross(rename[X](scan(A)), scan(B))"},
        {"A minus (B minus C)", "except(scan(A), except(scan(B), scan(C)))"},
        {"(A union B) intersect C", "intersect(union(scan(A), scan(B)), scan(C))"},
        {"A times (B join[k=k] C)", "cross(scan(A), join[(k = k)](scan(B), scan(C)))"},
        {"((R))", "scan(R)"},
    });
}

// --- Syntax errors ----------------------------------------------------------

TEST_CASE("leftover tokens after the query are a syntax error")
{
    checkSyntaxError("R )", 1, 3);
    checkSyntaxError("(R) S", 1, 5);
    checkSyntaxError("R union S T", 1, 11);
    checkSyntaxError("select[a=1](R) (S)", 1, 16);
    CHECK(parseError("R )")->message() == "Expected end of query, found ')'.");
}

TEST_CASE("two relation names in a row ask for a binary operator (Bonus 1 hint)")
{
    checkSyntaxError("A B", 1, 3);
    CHECK(parseError("A B")->message() == "Expected a binary operator between expressions, found 'B'.");
    checkSyntaxError("select[a=1](R S)", 1, 15);
}

TEST_CASE("a missing ')' is a syntax error at the token found instead (Bonus 1 case 16)")
{
    checkSyntaxError("select[Age>30](R", 1, 17);
    CHECK(parseError("select[Age>30](R")->message() == "Expected ')' after select expression, found end of input.");
    checkSyntaxError("(A union B", 1, 11);
    checkSyntaxError("distinct(A", 1, 11);
    checkSyntaxError("project[a](R ]", 1, 14);
}

TEST_CASE("a missing ']' is a syntax error")
{
    checkSyntaxError("select[Age>30(R)", 1, 14);
    CHECK(parseError("select[Age>30(R)")->message() == "Expected ']' after select condition, found '('.");
    checkSyntaxError("A join[k=k B", 1, 12);
    checkSyntaxError("project[a, b(R)", 1, 13);
    checkSyntaxError("rename[X(R)", 1, 9);
}

TEST_CASE("an empty projection list is a syntax error (Bonus 1 case 17)")
{
    checkSyntaxError("project[](R)", 1, 9);
    CHECK(parseError("project[](R)")->message() ==
          "Expected at least one column in the projection list, found ']'.");
    checkSyntaxError("project[a,](R)", 1, 11);
    checkSyntaxError("project[a.](R)", 1, 11);
}

TEST_CASE("an empty or keyword rename name is a syntax error")
{
    checkSyntaxError("rename[](R)", 1, 8);
    CHECK(parseError("rename[](R)")->message() == "Expected a relation name inside rename, found ']'.");
    checkSyntaxError("rename[union](R)", 1, 8);
}

TEST_CASE("other malformed queries give syntax errors with positions")
{
    checkSyntaxError("", 1, 1);
    checkSyntaxError("R union", 1, 8);
    checkSyntaxError("A join B", 1, 8);
    checkSyntaxError("select a=1 (R)", 1, 8);
    checkSyntaxError("select[a=1] R", 1, 13);
    checkSyntaxError("distinct R", 1, 10);
    checkSyntaxError("select[](R)", 1, 8);
    checkSyntaxError("A\nunion\n)", 3, 1);
    checkSyntaxError("union", 1, 1);
}

TEST_CASE("RA keywords are lowercase only, so SELECT[...] is a syntax error")
{
    checkSyntaxError("SELECT[a=1](R)", 1, 7);
    checkSyntaxError("R Union S", 1, 3);
    CHECK(tree("SELECT") == "scan(SELECT)");
}

TEST_CASE("lexical errors come from the tokenizer (Bonus 1 case 9)")
{
    const std::optional<db::Error> error = parseError("select[Name='Bob](R)");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Lexical);

    const std::optional<db::Error> brace = parseError("R union {S}");
    REQUIRE(brace.has_value());
    CHECK(brace->category() == ErrorCategory::Lexical);
}

// --- Depth limit ------------------------------------------------------------

namespace
{

    // `R op R op ... R` with `count` operators, e.g. chain("union", 2) is
    // `R union R union R`.
    std::string chain(const std::string& op, int count)
    {
        std::string out = "R";
        for (int i = 0; i < count; ++i)
        {
            out += " " + op + " R";
        }
        return out;
    }

    // Column of the `index`-th operator (1-based) in chain(op, ...).
    int chainColumn(const std::string& op, int index)
    {
        return 3 + (index - 1) * (static_cast<int>(op.size()) + 3);
    }

    void checkTooDeep(const std::string& query, const std::string& message, int column)
    {
        const std::optional<db::Error> error = parseError(query);
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Syntax);
        CHECK(error->message() == message);
        CHECK(error->line() == 1);
        CHECK(error->column() == column);
    }

    const std::string kQueryTooDeep = "Query nested deeper than 1000 levels.";
    const std::string kConditionTooDeep = "Condition nested deeper than 1000 levels.";
    const std::vector<std::string> kBinaryOperators = {"union", "minus", "intersect", "times", "join[k=k]"};

} // namespace

TEST_CASE("1000 levels of each kind parse")
{
    const int depth = db::kMaxNestingDepth;
    CHECK(tree(repeat("(", depth) + "R" + repeat(")", depth)) == "scan(R)");
    CHECK(tree(repeat("distinct(", depth) + "R" + repeat(")", depth)) ==
          repeat("distinct(", depth) + "scan(R)" + repeat(")", depth));
    CHECK(db::parseRaQuery(repeat("select[a=1](", depth / 2) + repeat("(", depth / 2) + "R" + repeat(")", depth))
              .op == db::PlanOp::Select);

    const std::vector<db::PlanOp> roots = {db::PlanOp::Union, db::PlanOp::Except, db::PlanOp::Intersect,
                                           db::PlanOp::Cross, db::PlanOp::Join};
    for (std::size_t i = 0; i < kBinaryOperators.size(); ++i)
    {
        INFO("operator: ", kBinaryOperators[i]);
        CHECK(db::parseRaQuery(chain(kBinaryOperators[i], depth)).op == roots[i]);
    }

    // Each `union (` adds two levels while parsing: the union node and the `(`.
    CHECK(db::parseRaQuery(repeat("A union (", depth / 2) + "R" + repeat(")", depth / 2)).op == db::PlanOp::Union);
}

TEST_CASE("level 1001 is a syntax error at the token that crosses the limit")
{
    const int depth = db::kMaxNestingDepth + 1;
    checkTooDeep(repeat("(", depth) + "R" + repeat(")", depth), kQueryTooDeep, depth);
    checkTooDeep(repeat("rename[X](", depth) + "R" + repeat(")", depth), kQueryTooDeep, 10 * (depth - 1) + 1);
    checkTooDeep(repeat("project[a](", depth / 2) + repeat("(", depth / 2 + 1) + "R" + repeat(")", depth),
                 kQueryTooDeep, 11 * (depth / 2) + depth / 2 + 1);

    for (const std::string& op : kBinaryOperators)
    {
        INFO("operator: ", op);
        checkTooDeep(chain(op, depth), kQueryTooDeep, chainColumn(op, depth));
    }

    checkTooDeep(repeat("A union (", 500) + "(R)" + repeat(")", 500), kQueryTooDeep, 9 * 500 + 1);
}

TEST_CASE("a binary operator counts the height of its left operand")
{
    const int depth = db::kMaxNestingDepth;
    const std::string deep = repeat("distinct(", depth) + "R" + repeat(")", depth);
    checkTooDeep(deep + " union R", kQueryTooDeep, static_cast<int>(deep.size()) + 2);
    checkTooDeep(deep + " join[k=k] R", kQueryTooDeep, static_cast<int>(deep.size()) + 2);

    // The right operand starts one level down, below the new node.
    CHECK(db::parseRaQuery("R union " + repeat("distinct(", depth - 1) + "R" + repeat(")", depth - 1)).op ==
          db::PlanOp::Union);
    checkTooDeep("R union " + repeat("distinct(", depth) + "R" + repeat(")", depth), kQueryTooDeep,
                 9 + 9 * (depth - 1));
}

TEST_CASE("the levels of a condition count in the height of its select or join node")
{
    const std::string join = "A join[" + repeat("not ", 999) + "k = k] B";
    CHECK(db::parseRaQuery(join).op == db::PlanOp::Join);
    checkTooDeep(join + " union C", kQueryTooDeep, static_cast<int>(join.size()) + 2);

    const std::string select = "select[" + repeat("not ", 999) + "a = 1](R)";
    CHECK(db::parseRaQuery(select).op == db::PlanOp::Select);
    checkTooDeep(select + " times C", kQueryTooDeep, static_cast<int>(select.size()) + 2);
}

TEST_CASE("plan and condition levels share one budget")
{
    // 599 `(` and the select node: 600 levels around the condition.
    const auto query = [](int conditionLevels)
    {
        return repeat("(", 599) + "select[" + repeat("not ", conditionLevels) + "a = 1](R)" + repeat(")", 599);
    };
    CHECK(db::parseRaQuery(query(400)).op == db::PlanOp::Select);
    checkTooDeep(query(401), kConditionTooDeep, 599 + 7 + 4 * 400 + 1);

    // A join node is one level below the plan above it.
    checkTooDeep("A join[" + repeat("not ", 1000) + "k = k] B", kConditionTooDeep, 8 + 4 * 999);

    // 500 query levels around a condition nested 1000 deep crashed the
    // first version of this parser; the budget makes it a syntax error.
    checkTooDeep(repeat("(", 500) + "select[" + repeat("(", 1000) + "a = 1" + repeat(")", 1000) + "](R)" +
                     repeat(")", 500),
                 kConditionTooDeep, 500 + 7 + 500);
}

TEST_CASE("100000 open parentheses give a syntax error, not a crash")
{
    checkTooDeep(repeat("(", 100000), kQueryTooDeep, db::kMaxNestingDepth + 1);
}

TEST_CASE("100000-operator chains give a syntax error, not a crash")
{
    for (const std::string& op : {std::string("union"), std::string("times"), std::string("join[k=k]")})
    {
        INFO("operator: ", op);
        checkTooDeep(chain(op, 100000), kQueryTooDeep, chainColumn(op, db::kMaxNestingDepth + 1));
    }
}
