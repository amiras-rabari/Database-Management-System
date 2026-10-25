// The condition tree: parseCondition, toString in both styles, the depth
// limit and the helpers (splitConjuncts, conjoin, columnsUsed, equiJoinKeys,
// mapColumns, replaceQualifier).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace
{

    using db::Column;
    using db::ColumnRef;
    using db::CompareOp;
    using db::Condition;
    using db::ConditionKind;
    using db::ConditionStyle;
    using db::ErrorCategory;
    using db::Schema;
    using db::Type;
    using db::Value;

    using KeyPairs = std::vector<std::pair<std::size_t, std::size_t>>;

    std::string plan(const std::string& text)
    {
        return db::toString(db::parseCondition(text), ConditionStyle::Plan);
    }

    std::string explicitStyle(const std::string& text)
    {
        return db::toString(db::parseCondition(text), ConditionStyle::Explicit);
    }

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

    std::optional<db::Error> parseError(const std::string& text)
    {
        return errorFrom([&] { db::parseCondition(text); });
    }

    void checkSyntaxError(const std::string& text, int line, int column)
    {
        INFO("input: ", text);
        const std::optional<db::Error> error = parseError(text);
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

    ColumnRef bare(const std::string& name)
    {
        return ColumnRef{std::nullopt, name};
    }

    ColumnRef qualified(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    Condition equals(const std::string& column, std::int32_t value)
    {
        return Condition::compare(bare(column), CompareOp::Equal, Value::ofInt(value));
    }

    // R(a INT, b INT)
    Schema rSchema()
    {
        return Schema({Column{"R", "a", Type::Int, 0}, Column{"R", "b", Type::Int, 0}});
    }

    // S(b INT, c INT)
    Schema sSchema()
    {
        return Schema({Column{"S", "b", Type::Int, 0}, Column{"S", "c", Type::Int, 0}});
    }

} // namespace

// --- Parsing and printing the Bonus 1 shapes --------------------------------

TEST_CASE("Bonus 1 condition shapes print in explicit style as the expected trees")
{
    // The bracket contents of the expected trees in tests/bonus1_cases.cpp.
    CHECK(explicitStyle("x1=3") == "(x1 = 3)");                                   // 1
    CHECK(explicitStyle(" x1 = 3 ") == "(x1 = 3)");                               // 2
    CHECK(explicitStyle("Age>=30") == "(Age >= 30)");                             // 3
    CHECK(explicitStyle("Age>-30") == "(Age > -30)");                             // 4
    CHECK(explicitStyle("Name='Bob)'") == "(Name = 'Bob)')");                     // 5
    CHECK(explicitStyle("Name='a,b'") == "(Name = 'a,b')");                       // 6
    CHECK(explicitStyle("Name='O''Brien'") == "(Name = 'O''Brien')");             // 7
    CHECK(explicitStyle("union=3") == "(union = 3)");                             // 8
    CHECK(explicitStyle("not (a=1 and b=2) or c>3") == "((not ((a = 1) and (b = 2))) or (c > 3))"); // 12
    CHECK(explicitStyle("a=1 and b=2 or c=3") == "(((a = 1) and (b = 2)) or (c = 3))");           // 13
    CHECK(explicitStyle("Age>30") == "(Age > 30)");                               // 14
    CHECK(explicitStyle("DID='D1'") == "(DID = 'D1')");                           // 14
}

TEST_CASE("plan style uses minimal parentheses")
{
    CHECK(plan("R.a > 500 and R.b = 'cs'") == "R.a > 500 and R.b = 'cs'");
    CHECK(plan("not (a=1 and b=2) or c>3") == "not (a = 1 and b = 2) or c > 3");
    CHECK(plan("a=1 and b=2 or c=3") == "a = 1 and b = 2 or c = 3");
    CHECK(plan("a=1 and (b=2 or c=3)") == "a = 1 and (b = 2 or c = 3)");
    CHECK(plan("(a=1 or b=2) and c=3") == "(a = 1 or b = 2) and c = 3");
    CHECK(plan("a = 1 and (b = 2 and c = 3)") == "a = 1 and (b = 2 and c = 3)");
    CHECK(plan("((a = 1))") == "a = 1");
    CHECK(plan("not a = 1") == "not a = 1");
    CHECK(plan("not not a = 1") == "not (not a = 1)");
}

TEST_CASE("printing uses canonical integers, <> and re-quoted strings")
{
    CHECK(plan("a = +7") == "a = 7");
    CHECK(plan("a = 007") == "a = 7");
    CHECK(plan("a != 1") == "a <> 1");
    CHECK(plan("a <> 1") == "a <> 1");
    CHECK(plan("n = 'it''s'") == "n = 'it''s'");
    CHECK(plan("1 = 'x'") == "1 = 'x'");
    CHECK(db::compareOpSymbol(CompareOp::LessEqual) == "<=");
    CHECK(db::compareOpSymbol(CompareOp::GreaterEqual) == ">=");
}

TEST_CASE("the dialect accepts keywords in any case and both not-equal forms")
{
    const Condition parsed = db::parseCondition("a = 1 AND b <> 2 Or NOT c = 3");
    CHECK(db::toString(parsed) == "a = 1 and b <> 2 or not c = 3");
    CHECK(parsed == db::parseCondition("a = 1 and b != 2 or not c = 3"));
}

TEST_CASE("operands are column refs, integers or strings")
{
    const Condition parsed = db::parseCondition("Emp.DID = 'D1'");
    REQUIRE(parsed.kind == ConditionKind::Compare);
    CHECK(parsed.op == CompareOp::Equal);
    CHECK(std::get<ColumnRef>(parsed.left) == qualified("Emp", "DID"));
    CHECK(std::get<Value>(parsed.right) == Value::ofVarchar("D1"));

    const Condition keywords = db::parseCondition("select.union < -5");
    CHECK(std::get<ColumnRef>(keywords.left) == qualified("select", "union"));
    CHECK(keywords.op == CompareOp::Less);
    CHECK(std::get<Value>(keywords.right) == Value::ofInt(-5));
}

// --- Precedence and associativity ---------------------------------------------

TEST_CASE("and binds tighter than or, not tighter than and")
{
    const Condition c = db::parseCondition("a = 1 or b = 2 and not c = 3");
    REQUIRE(c.kind == ConditionKind::Or);
    CHECK(c.children[0] == equals("a", 1));
    REQUIRE(c.children[1].kind == ConditionKind::And);
    CHECK(c.children[1].children[0] == equals("b", 2));
    CHECK(c.children[1].children[1] == Condition::notOf(equals("c", 3)));
}

TEST_CASE("and and or are left-associative")
{
    CHECK(db::parseCondition("a = 1 and b = 2 and c = 3") ==
          Condition::andOf(Condition::andOf(equals("a", 1), equals("b", 2)), equals("c", 3)));
    CHECK(db::parseCondition("a = 1 or b = 2 or c = 3") ==
          Condition::orOf(Condition::orOf(equals("a", 1), equals("b", 2)), equals("c", 3)));
    CHECK_FALSE(db::parseCondition("a = 1 and b = 2 and c = 3") ==
                db::parseCondition("a = 1 and (b = 2 and c = 3)"));
}

TEST_CASE("not applies to the next not_cond only")
{
    CHECK(db::parseCondition("not a = 1 and b = 2") ==
          Condition::andOf(Condition::notOf(equals("a", 1)), equals("b", 2)));
    CHECK(db::parseCondition("not (a = 1 and b = 2)") ==
          Condition::notOf(Condition::andOf(equals("a", 1), equals("b", 2))));
}

TEST_CASE("parse(toString(c)) == c in both styles")
{
    const std::vector<std::string> inputs = {
        "a = 1",
        "R.a > 500 and R.b = 'cs'",
        "a = 1 and b = 2 and c = 3",
        "a = 1 and (b = 2 and c = 3)",
        "a = 1 or b = 2 or c = 3",
        "a = 1 or (b = 2 or c = 3)",
        "(a = 1 or b = 2) and (c = 3 or d = 4)",
        "a = 1 and b = 2 or c = 3 and d = 4",
        "not (a=1 and b=2) or c>3",
        "not not a = 1",
        "not (not (a = 1 or not b = 2))",
        "x <> 'O''Brien' and y <= -2147483648 or z >= 2147483647",
        "union = 3 and Emp.select < 'x'",
    };
    for (const std::string& input : inputs)
    {
        INFO("input: ", input);
        const Condition c = db::parseCondition(input);
        CHECK(db::parseCondition(db::toString(c, ConditionStyle::Plan)) == c);
        CHECK(db::parseCondition(db::toString(c, ConditionStyle::Explicit)) == c);
    }
}

// --- Syntax errors ----------------------------------------------------------

TEST_CASE("syntax errors carry the position of the offending token")
{
    checkSyntaxError("", 1, 1);
    checkSyntaxError("a =", 1, 4);
    checkSyntaxError("a = 1 b = 2", 1, 7);
    checkSyntaxError("(a = 1", 1, 7);
    checkSyntaxError("a = 1)", 1, 6);
    checkSyntaxError("and = 1", 1, 1);
    checkSyntaxError("a b", 1, 3);
    checkSyntaxError("a.\n= 1", 2, 1);
    checkSyntaxError("a = 1 and", 1, 10);
}

TEST_CASE("syntax error messages name what was expected and what was found")
{
    CHECK(parseError("a = 1 b = 2")->message() == "Expected end of condition, found 'b'.");
    CHECK(parseError("(a = 1")->message() == "Expected ')' after condition, found end of input.");
    CHECK(parseError("and = 1")->message() == "Expected a column, integer or string, found 'and'.");
    CHECK(parseError("a 'x'")->message() == "Expected comparison operator, found string 'x'.");
}

TEST_CASE("lexical errors pass through parseCondition")
{
    const std::optional<db::Error> error = parseError("a = 'x");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Lexical);
}

// --- Depth limit ------------------------------------------------------------

TEST_CASE("nesting up to kMaxNestingDepth parses")
{
    const int depth = db::kMaxNestingDepth;
    CHECK(db::parseCondition(repeat("(", depth) + "a = 1" + repeat(")", depth)) == equals("a", 1));

    Condition expected = equals("a", 1);
    for (int i = 0; i < depth; ++i)
    {
        expected = Condition::notOf(std::move(expected));
    }
    CHECK(db::parseCondition(repeat("not ", depth) + "a = 1") == expected);
    CHECK(db::parseCondition(repeat("not (", depth / 2) + "a = 1" + repeat(")", depth / 2)).kind ==
          ConditionKind::Not);
}

TEST_CASE("nesting deeper than kMaxNestingDepth is a syntax error")
{
    const int depth = db::kMaxNestingDepth + 1;
    const std::optional<db::Error> parens = parseError(repeat("(", depth) + "a = 1" + repeat(")", depth));
    REQUIRE(parens.has_value());
    CHECK(parens->category() == ErrorCategory::Syntax);
    CHECK(parens->message() == "Condition nested deeper than 1000 levels.");
    CHECK(parens->column() == depth);

    const std::optional<db::Error> nots = parseError(repeat("not ", depth) + "a = 1");
    REQUIRE(nots.has_value());
    CHECK(nots->category() == ErrorCategory::Syntax);

    const std::optional<db::Error> mixed = parseError(repeat("not (", depth / 2 + 1) + "a = 1");
    REQUIRE(mixed.has_value());
    CHECK(mixed->category() == ErrorCategory::Syntax);
}

TEST_CASE("100000 open parentheses give a syntax error, not a crash")
{
    const std::optional<db::Error> error = parseError(repeat("(", 100000));
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Syntax);
    CHECK(error->message() == "Condition nested deeper than 1000 levels.");
}

namespace
{

    // `a = 1 op a = 1 op ... a = 1` with `count` operators.
    std::string conditionChain(const std::string& op, int count)
    {
        std::string out = "a = 1";
        for (int i = 0; i < count; ++i)
        {
            out += " " + op + " a = 1";
        }
        return out;
    }

    // Column of the `index`-th operator (1-based) in conditionChain(op, ...).
    int conditionChainColumn(const std::string& op, int index)
    {
        return 7 + (index - 1) * (static_cast<int>(op.size()) + 7);
    }

    void checkTooDeep(const std::string& text, int column)
    {
        INFO("length: ", text.size());
        const std::optional<db::Error> error = parseError(text);
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Syntax);
        CHECK(error->message() == "Condition nested deeper than 1000 levels.");
        CHECK(error->column() == column);
    }

} // namespace

TEST_CASE("a chain of 1000 and/or parses; the 1001st operator is a syntax error")
{
    const int depth = db::kMaxNestingDepth;
    CHECK(db::parseCondition(conditionChain("and", depth)).kind == ConditionKind::And);
    CHECK(db::parseCondition(conditionChain("or", depth)).kind == ConditionKind::Or);
    checkTooDeep(conditionChain("and", depth + 1), conditionChainColumn("and", depth + 1));
    checkTooDeep(conditionChain("or", depth + 1), conditionChainColumn("or", depth + 1));
}

TEST_CASE("an and/or counts the height of its left operand")
{
    const int depth = db::kMaxNestingDepth;
    checkTooDeep(repeat("not ", depth) + "a = 1 and b = 2", 4 * depth + 7);
    checkTooDeep(repeat("not ", depth) + "a = 1 or b = 2", 4 * depth + 7);

    // The right operand starts one level down, below the new node.
    CHECK(db::parseCondition("a = 1 and " + repeat("not ", depth - 1) + "b = 2").kind == ConditionKind::And);
    checkTooDeep("a = 1 and " + repeat("not ", depth) + "b = 2", 11 + 4 * (depth - 1));
}

TEST_CASE("100000-term and/or chains give a syntax error, not a crash")
{
    checkTooDeep(conditionChain("and", 100000), conditionChainColumn("and", db::kMaxNestingDepth + 1));
    checkTooDeep(conditionChain("or", 100000), conditionChainColumn("or", db::kMaxNestingDepth + 1));
}

// --- Helpers ----------------------------------------------------------------

TEST_CASE("splitConjuncts flattens every and, left to right")
{
    const std::vector<Condition> parts = db::splitConjuncts(db::parseCondition("a=1 and (b=2 and c=3)"));
    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == equals("a", 1));
    CHECK(parts[1] == equals("b", 2));
    CHECK(parts[2] == equals("c", 3));

    CHECK(db::splitConjuncts(db::parseCondition("a=1 or b=2")).size() == 1);
    CHECK(db::splitConjuncts(db::parseCondition("not (a=1 and b=2)")).size() == 1);
}

TEST_CASE("conjoin joins parts left-nested")
{
    const std::vector<Condition> parts = db::splitConjuncts(db::parseCondition("a=1 and (b=2 and c=3)"));
    CHECK(db::conjoin(parts) == db::parseCondition("a=1 and b=2 and c=3"));
    CHECK(db::conjoin({equals("a", 1)}) == equals("a", 1));
    CHECK_THROWS_AS(db::conjoin({}), std::logic_error);
}

TEST_CASE("columnsUsed lists each column once in order of first appearance")
{
    const std::vector<ColumnRef> columns =
        db::columnsUsed(db::parseCondition("R.b = S.b and (a > 1 or R.b < a) and 1 = 1 and not S.b = c"));
    CHECK(columns == std::vector<ColumnRef>{qualified("R", "b"), qualified("S", "b"), bare("a"), bare("c")});
    CHECK(db::columnsUsed(db::parseCondition("1 = 2")).empty());
    CHECK(db::columnsUsed(db::parseCondition("R.a = a")).size() == 2);
}

TEST_CASE("equiJoinKeys returns (left, right) index pairs in either orientation")
{
    const Schema r = rSchema();
    const Schema s = sSchema();
    CHECK(db::equiJoinKeys(db::parseCondition("R.b = S.b"), r, s) == KeyPairs{{1, 0}});
    CHECK(db::equiJoinKeys(db::parseCondition("S.b = R.b"), r, s) == KeyPairs{{1, 0}});
    CHECK(db::equiJoinKeys(db::parseCondition("R.b = S.b and S.c = R.a"), r, s) == KeyPairs{{1, 0}, {0, 1}});
    CHECK(db::equiJoinKeys(db::parseCondition("a = c"), r, s) == KeyPairs{{0, 1}});
}

TEST_CASE("equiJoinKeys is nullopt unless every conjunct is a left = right equality")
{
    const Schema r = rSchema();
    const Schema s = sSchema();
    CHECK_FALSE(db::equiJoinKeys(db::parseCondition("R.a = R.b"), r, s).has_value());
    CHECK_FALSE(db::equiJoinKeys(db::parseCondition("R.b = S.b or R.a = S.c"), r, s).has_value());
    CHECK_FALSE(db::equiJoinKeys(db::parseCondition("R.b = S.b and R.a > 5"), r, s).has_value());
    CHECK_FALSE(db::equiJoinKeys(db::parseCondition("R.b < S.b"), r, s).has_value());
    CHECK_FALSE(db::equiJoinKeys(db::parseCondition("R.b = 3"), r, s).has_value());
    CHECK_FALSE(db::equiJoinKeys(db::parseCondition("not R.b = S.b"), r, s).has_value());
}

TEST_CASE("equiJoinKeys raises the column resolution errors")
{
    const Schema r = rSchema();
    const Schema s = sSchema();
    const std::optional<db::Error> ambiguous =
        errorFrom([&] { db::equiJoinKeys(db::parseCondition("b = S.c"), r, s); });
    REQUIRE(ambiguous.has_value());
    CHECK(ambiguous->category() == ErrorCategory::Ambiguity);

    const std::optional<db::Error> unknown =
        errorFrom([&] { db::equiJoinKeys(db::parseCondition("R.b = S.x"), r, s); });
    REQUIRE(unknown.has_value());
    CHECK(unknown->category() == ErrorCategory::Name);
}

TEST_CASE("mapColumns replaces every column ref and keeps the rest")
{
    const Condition mapped = db::mapColumns(db::parseCondition("a = 1 or not (b < a and 'x' = c)"),
                                            [](const ColumnRef& ref)
                                            {
                                                return ColumnRef{std::string("T"), ref.name + "2"};
                                            });
    CHECK(mapped == db::parseCondition("T.a2 = 1 or not (T.b2 < T.a2 and 'x' = T.c2)"));
}

TEST_CASE("replaceQualifier renames one qualifier only")
{
    CHECK(db::replaceQualifier(db::parseCondition("e.name = 'x'"), "e", "Emp") ==
          db::parseCondition("Emp.name = 'x'"));
    CHECK(db::replaceQualifier(db::parseCondition("e.a = d.a and a = e.b"), "e", "Emp") ==
          db::parseCondition("Emp.a = d.a and a = Emp.b"));
}
