// Tests for the condition grammar (CHUNK_03 section 4): columns, operands,
// comparisons, and the NOT > AND > OR layering of §M3.2, through the
// internal hook parseConditionText (chunk 04 adds the public entry points).

#include "parser/sql_parser.h"

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/value.h>
#include <db/sql/ast.h>
#include <db/sql/source_pos.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <variant>

using db::CompareOp;
using db::ConditionKind;
using db::Error;
using db::ErrorCategory;
using db::Value;
using db::sql::AggregateCall;
using db::sql::ColumnName;
using db::sql::Literal;
using db::sql::SourcePos;
using db::sql::SqlCondition;
using db::sql::parseConditionText;

namespace
{

    // Checks that a node is the comparison `<bare column> op <INT literal>`.
    void checkCompare(const SqlCondition& c, const std::string& column, CompareOp op, int32_t value)
    {
        REQUIRE(c.kind == ConditionKind::Compare);
        CHECK(c.op == op);
        REQUIRE(std::holds_alternative<ColumnName>(c.left));
        CHECK(std::get<ColumnName>(c.left).name == column);
        REQUIRE(std::holds_alternative<Literal>(c.right));
        CHECK(std::get<Literal>(c.right).value == Value::ofInt(value));
    }

    // Parses text that must fail, and returns the syntax error.
    Error syntaxErrorOf(const std::string& text)
    {
        try
        {
            parseConditionText(text);
        }
        catch (const Error& error)
        {
            CHECK_MESSAGE(error.category() == ErrorCategory::Syntax, text);
            return error;
        }
        FAIL("expected a syntax error for: " << text);
        return Error(ErrorCategory::Syntax, "unreachable");
    }

    void checkErrorAt(const std::string& text, int line, int column, const std::string& message)
    {
        Error error = syntaxErrorOf(text);
        CHECK_MESSAGE(error.line() == line, text);
        CHECK_MESSAGE(error.column() == column, text);
        CHECK(error.message() == message);
    }

    // True when the parser accepts the text, false on a db::Error.
    bool sqlAccepts(const std::string& text)
    {
        try
        {
            parseConditionText(text);
            return true;
        }
        catch (const Error&)
        {
            return false;
        }
    }

    // The same question for common's condition parser (the reference for
    // the depth budget, kMaxNestingDepth).
    bool commonAccepts(const std::string& text)
    {
        try
        {
            db::parseCondition(text);
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

TEST_CASE("age > 30: the WHERE of example 5.1")
{
    SqlCondition c = parseConditionText("age > 30");
    checkCompare(c, "age", CompareOp::Greater, 30);
    CHECK(c.pos == SourcePos{1, 1});
    CHECK_FALSE(std::get<ColumnName>(c.left).qualifier.has_value());
    CHECK(std::get<Literal>(c.right).pos == SourcePos{1, 7});
}

TEST_CASE("e.did = d.did: qualified columns on both sides (example 5.2's ON)")
{
    SqlCondition c = parseConditionText("e.did = d.did");
    REQUIRE(c.kind == ConditionKind::Compare);
    const ColumnName& left = std::get<ColumnName>(c.left);
    const ColumnName& right = std::get<ColumnName>(c.right);
    CHECK(left.qualifier == "e");
    CHECK(left.name == "did");
    CHECK(right.qualifier == "d");
    CHECK(right.name == "did");
    CHECK(right.pos == SourcePos{1, 9});
}

TEST_CASE("precedence NOT > AND > OR comes from the grammar layers")
{
    // a = 1 OR b = 2 AND NOT c = 3  ->  Or(a=1, And(b=2, Not(c=3)))
    SqlCondition c = parseConditionText("a = 1 OR b = 2 AND NOT c = 3");
    REQUIRE(c.kind == ConditionKind::Or);
    REQUIRE(c.children.size() == 2);
    checkCompare(c.children[0], "a", CompareOp::Equal, 1);
    const SqlCondition& andNode = c.children[1];
    REQUIRE(andNode.kind == ConditionKind::And);
    REQUIRE(andNode.children.size() == 2);
    checkCompare(andNode.children[0], "b", CompareOp::Equal, 2);
    const SqlCondition& notNode = andNode.children[1];
    REQUIRE(notNode.kind == ConditionKind::Not);
    CHECK(notNode.pos == SourcePos{1, 20});
    REQUIRE(notNode.children.size() == 1);
    checkCompare(notNode.children[0], "c", CompareOp::Equal, 3);
}

TEST_CASE("parentheses override precedence")
{
    SqlCondition c = parseConditionText("(a = 1 OR b = 2) AND c = 3");
    REQUIRE(c.kind == ConditionKind::And);
    REQUIRE(c.children.size() == 2);
    CHECK(c.children[0].kind == ConditionKind::Or);
    checkCompare(c.children[1], "c", CompareOp::Equal, 3);
}

TEST_CASE("AND and OR chains nest to the left, like common's andOf/orOf")
{
    // a = 1 AND b = 2 AND c = 3  ->  And(And(a=1, b=2), c=3)
    SqlCondition c = parseConditionText("a = 1 AND b = 2 AND c = 3");
    REQUIRE(c.kind == ConditionKind::And);
    REQUIRE(c.children.size() == 2);
    REQUIRE(c.children[0].kind == ConditionKind::And);
    checkCompare(c.children[0].children[0], "a", CompareOp::Equal, 1);
    checkCompare(c.children[0].children[1], "b", CompareOp::Equal, 2);
    checkCompare(c.children[1], "c", CompareOp::Equal, 3);
    CHECK(c.pos == SourcePos{1, 1});

    SqlCondition o = parseConditionText("a = 1 OR b = 2 OR c = 3");
    REQUIRE(o.kind == ConditionKind::Or);
    CHECK(o.children[0].kind == ConditionKind::Or);
}

TEST_CASE("a parsed AND node equals the same node built by hand")
{
    // The leading space keeps every position away from the default (1, 1),
    // so leftovers of the move inside makeBinary would show up.
    //  a = 1 AND b = 2
    // 1234567890123456
    SqlCondition left;
    left.kind = ConditionKind::Compare;
    left.left = ColumnName{{}, "a", {1, 2}};
    left.right = Literal{Value::ofInt(1), {1, 6}};
    left.pos = {1, 2};
    SqlCondition right;
    right.kind = ConditionKind::Compare;
    right.left = ColumnName{{}, "b", {1, 12}};
    right.right = Literal{Value::ofInt(2), {1, 16}};
    right.pos = {1, 12};
    SqlCondition expected;
    expected.kind = ConditionKind::And;
    expected.pos = {1, 2};
    expected.children = {left, right};
    CHECK(parseConditionText(" a = 1 AND b = 2") == expected);
}

TEST_CASE("NOT NOT a = 1")
{
    SqlCondition c = parseConditionText("NOT NOT a = 1");
    REQUIRE(c.kind == ConditionKind::Not);
    REQUIRE(c.children[0].kind == ConditionKind::Not);
    checkCompare(c.children[0].children[0], "a", CompareOp::Equal, 1);
}

TEST_CASE("operators and literal types")
{
    SqlCondition ne = parseConditionText("a <> 'x'");
    CHECK(ne.op == CompareOp::NotEqual);
    CHECK(std::get<Literal>(ne.right).value == Value::ofVarchar("x"));

    checkCompare(parseConditionText("a >= -5"), "a", CompareOp::GreaterEqual, -5);
    checkCompare(parseConditionText("a <= 1"), "a", CompareOp::LessEqual, 1);
    checkCompare(parseConditionText("a < 1"), "a", CompareOp::Less, 1);

    // A literal may be on the left too: operand ::= column | int | string.
    SqlCondition lit = parseConditionText("1 = 1");
    CHECK(std::holds_alternative<Literal>(lit.left));
}

TEST_CASE("COUNT(*) > 5 has an aggregate operand (the HAVING of example 5.4)")
{
    SqlCondition c = parseConditionText("COUNT(*) > 5");
    REQUIRE(std::holds_alternative<AggregateCall>(c.left));
    const AggregateCall& call = std::get<AggregateCall>(c.left);
    CHECK(call.function == db::AggregateFunction::Count);
    CHECK_FALSE(call.argument.has_value());
    CHECK(call.pos == SourcePos{1, 1});

    SqlCondition avg = parseConditionText("avg(e.salary) > 5");
    const AggregateCall& avgCall = std::get<AggregateCall>(avg.left);
    CHECK(avgCall.function == db::AggregateFunction::Avg);
    REQUIRE(avgCall.argument.has_value());
    CHECK(avgCall.argument->qualifier == "e");
    CHECK(avgCall.argument->name == "salary");
}

TEST_CASE("syntax errors at the unexpected token")
{
    checkErrorAt("a = ", 1, 5, "expected an operand but found end of input");
    checkErrorAt("a 5", 1, 3, "expected a comparison operator (=, <>, <, <=, >, >=) but found '5'");
    checkErrorAt("(a = 1", 1, 7, "expected ')' but found end of input");
    checkErrorAt("a = 1 AND", 1, 10, "expected an operand but found end of input");
    checkErrorAt("e.", 1, 3, "expected a column name after '.' but found end of input");
    checkErrorAt("a = 1 b", 1, 7, "expected end of input but found 'b'");
    checkErrorAt("COUNT(a b) > 1", 1, 9, "expected ')' but found 'b'");
    checkErrorAt("SUM() > 1", 1, 5, "expected a column or '*' but found ')'");
}

TEST_CASE("nesting depth matches common's budget (kMaxNestingDepth)")
{
    for (int depth : {999, 1000, 1001})
    {
        const std::string parens = repeated("(", depth) + "a = 1" + repeated(")", depth);
        CHECK_MESSAGE(sqlAccepts(parens) == commonAccepts(parens), "parens depth " << depth);
        const std::string nots = repeated("NOT ", depth) + "a = 1";
        CHECK_MESSAGE(sqlAccepts(nots) == commonAccepts(nots), "NOT depth " << depth);
    }
    // Too deep is a syntax error, never a crash.
    Error error = syntaxErrorOf(repeated("(", 5000) + "a = 1" + repeated(")", 5000));
    CHECK(error.message() == "nesting too deep");
    // A very long AND chain is refused too (it nests to the left).
    CHECK_FALSE(sqlAccepts("a = 1" + repeated(" AND a = 1", 2000)));
    CHECK(sqlAccepts("a = 1" + repeated(" AND a = 1", 100)));
}
