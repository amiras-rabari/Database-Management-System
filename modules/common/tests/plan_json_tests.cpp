// Plan JSON: the spec's examples field for field, a round trip of every node
// type, field order, and every way fromJson rejects a plan.

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/value.h"

namespace
{

    using db::ColumnRef;
    using db::ErrorCategory;
    using db::PlanJson;
    using db::PlanNode;

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

    void checkRejected(const std::string& jsonText, ErrorCategory category, const std::string& message)
    {
        INFO("json: ", jsonText);
        const std::optional<db::Error> error = errorFrom([&] { db::fromJson(PlanJson::parse(jsonText)); });
        REQUIRE(error.has_value());
        CHECK(error->category() == category);
        CHECK(error->message() == message);
    }

    ColumnRef qualified(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    PlanNode scan(const std::string& table)
    {
        return PlanNode::scan(table);
    }

    // One plan per node type.
    std::vector<PlanNode> onePlanPerOp()
    {
        return {
            scan("R"),
            PlanNode::select(db::parseCondition("R.a > 500 and R.b = 'cs'"), scan("R")),
            PlanNode::project({qualified("R", "a"), ColumnRef{std::nullopt, "b"}}, scan("R")),
            PlanNode::join(db::parseCondition("R.b = S.b"), scan("R"), scan("S")),
            PlanNode::cross(scan("R"), scan("S")),
            PlanNode::rename("e", scan("Emp")),
            PlanNode::unionOf(scan("R"), scan("S")),
            PlanNode::intersect(scan("R"), scan("S")),
            PlanNode::except(scan("R"), scan("S")),
            PlanNode::distinct(scan("R")),
        };
    }

    std::vector<std::string> keysOf(const PlanJson& json)
    {
        std::vector<std::string> keys;
        for (const auto& item : json.items())
        {
            keys.push_back(item.key());
        }
        return keys;
    }

    // `distinct` wrapped `levels` times around a scan.
    PlanNode distinctChain(int levels)
    {
        PlanNode plan = scan("R");
        for (int i = 0; i < levels; ++i)
        {
            plan = PlanNode::distinct(std::move(plan));
        }
        return plan;
    }

    std::string repeat(const std::string& text, int times)
    {
        std::string out;
        out.reserve(text.size() * static_cast<std::size_t>(times));
        for (int i = 0; i < times; ++i)
        {
            out += text;
        }
        return out;
    }

    // `levels` nested distinct nodes around `inner`, as JSON text.
    std::string distinctChainText(int levels, const std::string& inner)
    {
        return repeat("{\"op\":\"distinct\",\"child\":", levels) + inner + repeat("}", levels);
    }

    // sql/SPEC.md example 5.1 (SELECT name, age FROM Emp WHERE age > 30).
    const char* const kSpecExample51 = R"({ "op":"project", "columns":["Emp.name","Emp.age"],
      "child": { "op":"select", "condition":"Emp.age > 30",
                 "child": { "op":"scan", "table":"Emp" } } })";

    // sql/SPEC.md example 5.2 (join of two renamed tables).
    const char* const kSpecExample52 = R"({ "op":"project", "columns":["e.name","d.title"],
      "child": { "op":"select", "condition":"d.budget > 1000",
        "child": { "op":"join", "condition":"e.did = d.did",
          "left":  { "op":"rename", "to":"e",
                     "child": { "op":"scan", "table":"Emp" } },
          "right": { "op":"rename", "to":"d",
                     "child": { "op":"scan", "table":"Dept" } } } } })";

    PlanNode specPlan51()
    {
        return PlanNode::project({qualified("Emp", "name"), qualified("Emp", "age")},
                                 PlanNode::select(db::parseCondition("Emp.age > 30"), scan("Emp")));
    }

    PlanNode specPlan52()
    {
        return PlanNode::project(
            {qualified("e", "name"), qualified("d", "title")},
            PlanNode::select(db::parseCondition("d.budget > 1000"),
                             PlanNode::join(db::parseCondition("e.did = d.did"), PlanNode::rename("e", scan("Emp")),
                                            PlanNode::rename("d", scan("Dept")))));
    }

} // namespace

TEST_CASE("toJson writes the spec's example 5.1 field for field and in order")
{
    CHECK(db::toJson(specPlan51()).dump() == PlanJson::parse(kSpecExample51).dump());
}

TEST_CASE("toJson writes the spec's example 5.2: rename uses 'to', join uses left and right")
{
    CHECK(db::toJson(specPlan52()).dump() == PlanJson::parse(kSpecExample52).dump());
}

TEST_CASE("fromJson reads the spec's examples back to the same plans")
{
    CHECK(db::fromJsonText(kSpecExample51) == specPlan51());
    CHECK(db::fromJsonText(kSpecExample52) == specPlan52());
}

TEST_CASE("every node type round-trips through JSON and through JSON text")
{
    for (const PlanNode& plan : onePlanPerOp())
    {
        INFO("plan: ", db::toString(plan));
        CHECK(db::fromJson(db::toJson(plan)) == plan);
        CHECK(db::fromJsonText(db::toJsonText(plan)) == plan);
        CHECK(db::fromJsonText(db::toJsonText(plan, -1)) == plan);
    }
}

TEST_CASE("a plan using every node type at once round-trips")
{
    const PlanNode plan = PlanNode::distinct(PlanNode::except(
        PlanNode::unionOf(
            PlanNode::project({qualified("e", "a")},
                              PlanNode::select(db::parseCondition("not (e.a = 1 or e.b <> 'x''y')"),
                                               PlanNode::rename("e", scan("R")))),
            PlanNode::project({ColumnRef{std::nullopt, "a"}},
                              PlanNode::join(db::parseCondition("R.a = S.a"), scan("R"), scan("S")))),
        PlanNode::intersect(PlanNode::cross(scan("T"), scan("U")), scan("V"))));
    CHECK(db::fromJsonText(db::toJsonText(plan)) == plan);
}

TEST_CASE("each node writes op first, then its own data, then its inputs")
{
    const std::vector<std::vector<std::string>> expected = {
        {"op", "table"},
        {"op", "condition", "child"},
        {"op", "columns", "child"},
        {"op", "condition", "left", "right"},
        {"op", "left", "right"},
        {"op", "to", "child"},
        {"op", "left", "right"},
        {"op", "left", "right"},
        {"op", "left", "right"},
        {"op", "child"},
    };
    const std::vector<PlanNode> plans = onePlanPerOp();
    REQUIRE(plans.size() == expected.size());
    for (std::size_t i = 0; i < plans.size(); ++i)
    {
        INFO("plan: ", db::toString(plans[i]));
        CHECK(keysOf(db::toJson(plans[i])) == expected[i]);
    }
}

TEST_CASE("op names in JSON are the M3 logical names")
{
    std::vector<std::string> names;
    for (const PlanNode& plan : onePlanPerOp())
    {
        names.push_back(db::toJson(plan).at("op").get<std::string>());
    }
    CHECK(names == std::vector<std::string>{"scan", "select", "project", "join", "cross", "rename", "union",
                                            "intersect", "except", "distinct"});
}

TEST_CASE("a condition is written in plan style and read in any dialect")
{
    const PlanNode plan =
        db::fromJsonText(R"({"op":"select","condition":"R.a>500 AND R.b='cs'","child":{"op":"scan","table":"R"}})");
    CHECK(db::toJsonText(plan, -1) ==
          R"({"op":"select","condition":"R.a > 500 and R.b = 'cs'","child":{"op":"scan","table":"R"}})");
}

TEST_CASE("column strings may be bare, qualified, or RA keywords")
{
    const PlanNode plan = db::fromJsonText(
        R"({"op":"project","columns":["a","R.b","union"],"child":{"op":"scan","table":"R"}})");
    CHECK(plan.columns == std::vector<ColumnRef>{ColumnRef{std::nullopt, "a"}, qualified("R", "b"),
                                                 ColumnRef{std::nullopt, "union"}});
}

TEST_CASE("toJsonText is one line for a negative indent and pretty-printed otherwise")
{
    const PlanNode plan = PlanNode::distinct(scan("R"));
    CHECK(db::toJsonText(plan, -1) == R"({"op":"distinct","child":{"op":"scan","table":"R"}})");
    CHECK(db::toJsonText(plan, 2) == "{\n  \"op\": \"distinct\",\n  \"child\": {\n    \"op\": \"scan\",\n"
                                     "    \"table\": \"R\"\n  }\n}");
}

TEST_CASE("fromJson rejects a plan that is not an object, or a node without a usable op")
{
    checkRejected("[]", ErrorCategory::Schema, "Plan must be a JSON object.");
    checkRejected("{}", ErrorCategory::Schema, "Plan node is missing required field 'op'.");
    checkRejected(R"({"op":5})", ErrorCategory::Schema, "Plan node field 'op' must be a string.");
    checkRejected(R"({"op":"sort","keys":[]})", ErrorCategory::Schema, "Unknown plan operator 'sort'.");
    checkRejected(R"({"op":"seqscan","table":"R"})", ErrorCategory::Schema, "Unknown plan operator 'seqscan'.");
}

TEST_CASE("fromJson names the node and the missing or unexpected field")
{
    checkRejected(R"({"op":"select","child":{"op":"scan","table":"R"}})", ErrorCategory::Schema,
                  "Node 'select': missing required field 'condition'.");
    checkRejected(R"({"op":"join","condition":"a = b","left":{"op":"scan","table":"R"}})", ErrorCategory::Schema,
                  "Node 'join': missing required field 'right'.");
    checkRejected(R"({"op":"scan","table":"R","mode":"hash"})", ErrorCategory::Schema,
                  "Node 'scan': unexpected field 'mode'.");
    checkRejected(R"({"op":"distinct","child":{"op":"scan","table":"R"},"left":{}})", ErrorCategory::Schema,
                  "Node 'distinct': unexpected field 'left'.");
}

TEST_CASE("fromJson names the inner node when the problem is deep in the tree")
{
    checkRejected(
        R"({"op":"join","condition":"R.a = S.a","left":{"op":"scan","table":"R"},"right":{"op":"scan"}})",
        ErrorCategory::Schema, "Node 'scan': missing required field 'table'.");
}

TEST_CASE("fromJson checks the type and content of every field")
{
    checkRejected(R"({"op":"scan","table":7})", ErrorCategory::Schema, "Node 'scan': field 'table' must be a string.");
    checkRejected(R"({"op":"scan","table":""})", ErrorCategory::Schema,
                  "Node 'scan': field 'table' must not be empty.");
    checkRejected(R"({"op":"rename","to":"","child":{"op":"scan","table":"R"}})", ErrorCategory::Schema,
                  "Node 'rename': field 'to' must not be empty.");
    checkRejected(R"({"op":"select","condition":5,"child":{"op":"scan","table":"R"}})", ErrorCategory::Schema,
                  "Node 'select': field 'condition' must be a string.");
    checkRejected(R"({"op":"select","condition":"a = 1","child":"R"})", ErrorCategory::Schema,
                  "Node 'select': field 'child' must be an object.");
    checkRejected(R"({"op":"union","left":{"op":"scan","table":"R"},"right":[]})", ErrorCategory::Schema,
                  "Node 'union': field 'right' must be an object.");
}

TEST_CASE("fromJson checks the projection column list")
{
    const std::string child = R"(,"child":{"op":"scan","table":"R"}})";
    checkRejected(R"({"op":"project","columns":"a")" + child, ErrorCategory::Schema,
                  "Node 'project': field 'columns' must be an array of strings.");
    checkRejected(R"({"op":"project","columns":[1])" + child, ErrorCategory::Schema,
                  "Node 'project': field 'columns' must be an array of strings.");
    checkRejected(R"({"op":"project","columns":[])" + child, ErrorCategory::Schema,
                  "Node 'project': field 'columns' must not be empty.");
    checkRejected(R"({"op":"project","columns":["a b"])" + child, ErrorCategory::Schema,
                  "Node 'project': invalid column 'a b'.");
    checkRejected(R"({"op":"project","columns":["R."])" + child, ErrorCategory::Schema,
                  "Node 'project': invalid column 'R.'.");
    checkRejected(R"({"op":"project","columns":["and"])" + child, ErrorCategory::Schema,
                  "Node 'project': invalid column 'and'.");
    checkRejected(R"({"op":"project","columns":["'a"])" + child, ErrorCategory::Schema,
                  "Node 'project': invalid column ''a'.");
}

TEST_CASE("a condition that does not parse keeps its category and names the node")
{
    const std::string child = R"(,"child":{"op":"scan","table":"R"}})";
    checkRejected(R"({"op":"select","condition":"a =")" + child, ErrorCategory::Syntax,
                  "Node 'select': invalid condition \"a =\": Expected a column, integer or string, "
                  "found end of input. (line 1, column 4 of the condition)");
    const std::optional<db::Error> lexical = errorFrom(
        [&] { db::fromJsonText(R"({"op":"select","condition":"a = 'x")" + child); });
    REQUIRE(lexical.has_value());
    CHECK(lexical->category() == ErrorCategory::Lexical);
    CHECK(lexical->message().rfind("Node 'select': invalid condition", 0) == 0);
    checkRejected(R"({"op":"join","condition":"a = 1 b","left":{"op":"scan","table":"R"},)"
                  R"("right":{"op":"scan","table":"S"}})",
                  ErrorCategory::Syntax,
                  "Node 'join': invalid condition \"a = 1 b\": Expected end of condition, found 'b'. "
                  "(line 1, column 7 of the condition)");
}

TEST_CASE("schema errors from fromJson carry no position")
{
    const std::optional<db::Error> error = errorFrom([] { db::fromJsonText(R"({"op":"scan"})"); });
    REQUIRE(error.has_value());
    CHECK(error->line() == 0);
}

TEST_CASE("text that is not JSON is a syntax error at its line and column")
{
    const std::optional<db::Error> empty = errorFrom([] { db::fromJsonText(""); });
    REQUIRE(empty.has_value());
    CHECK(empty->category() == ErrorCategory::Syntax);
    CHECK(empty->line() == 1);

    const std::optional<db::Error> broken = errorFrom([] { db::fromJsonText("{\"op\": \"scan\",\n  \"table\": }"); });
    REQUIRE(broken.has_value());
    CHECK(broken->category() == ErrorCategory::Syntax);
    CHECK(broken->line() == 2);
    CHECK(broken->column() == 12);
    CHECK(broken->message().rfind("Malformed plan JSON: ", 0) == 0);
    CHECK(broken->message().find("json.exception") == std::string::npos);
}

TEST_CASE("plans up to kMaxNestingDepth levels round-trip; one more level is rejected")
{
    const PlanNode deepest = distinctChain(db::kMaxNestingDepth);
    CHECK(db::fromJsonText(db::toJsonText(deepest, -1)) == deepest);

    const std::optional<db::Error> tooDeep =
        errorFrom([] { db::fromJsonText(distinctChainText(db::kMaxNestingDepth + 1, R"({"op":"scan","table":"R"})")); });
    REQUIRE(tooDeep.has_value());
    CHECK(tooDeep->category() == ErrorCategory::Schema);
    CHECK(tooDeep->message() == "Node 'distinct': plan nested deeper than 1000 levels.");
}

TEST_CASE("a 100000-level plan text is rejected, not a stack overflow")
{
    const std::optional<db::Error> error =
        errorFrom([] { db::fromJsonText(distinctChainText(100000, R"({"op":"scan","table":"R"})")); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

TEST_CASE("plan levels and condition levels share one budget")
{
    // A select at level 600 (599 distinct nodes above it, plus itself) may
    // hold a condition 400 levels deep, and no deeper.
    const auto selectText = [](int parens)
    {
        const std::string condition = repeat("(", parens) + "a = 1" + repeat(")", parens);
        return distinctChainText(599, R"({"op":"select","condition":")" + condition +
                                          R"(","child":{"op":"scan","table":"R"}})");
    };
    CHECK_NOTHROW(db::fromJsonText(selectText(400)));

    const std::optional<db::Error> error = errorFrom([&] { db::fromJsonText(selectText(401)); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Syntax);
    CHECK(error->message().find("Condition nested deeper than 1000 levels.") != std::string::npos);
}

TEST_CASE("toJsonText rejects a string that is not valid UTF-8; toJson accepts it")
{
    const PlanNode plan = PlanNode::select(
        db::Condition::compare(ColumnRef{std::nullopt, "a"}, db::CompareOp::Equal,
                               db::Value::ofVarchar(std::string(1, static_cast<char>(0xC3)))),
        scan("R"));
    CHECK_NOTHROW(db::toJson(plan));
    const std::optional<db::Error> error = errorFrom([&] { db::toJsonText(plan); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

TEST_CASE("toJson of a malformed node is an internal error")
{
    PlanNode noCondition = PlanNode::select(db::parseCondition("a = 1"), scan("R"));
    noCondition.condition.reset();
    CHECK_THROWS_AS(db::toJson(noCondition), std::logic_error);

    PlanNode noInput = PlanNode::distinct(scan("R"));
    noInput.children.clear();
    CHECK_THROWS_AS(db::toJson(noInput), std::logic_error);
}
