// The reference operators over TupleSource: results and schemas, the two
// counters, rewind, set-operation order, construction-time errors, and
// buildOperators (including a 1000-level plan).

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/operators.h"
#include "db/common/plan.h"
#include "db/common/ra_parser.h"
#include "db/common/schema.h"
#include "db/common/tuple_source.h"
#include "db/common/value.h"
#include "vector_source.h"

namespace
{

    using db::Column;
    using db::ColumnRef;
    using db::ErrorCategory;
    using db::OperatorCounters;
    using db::PlanNode;
    using db::Schema;
    using db::Tuple;
    using db::TupleSourcePtr;
    using db::Type;
    using db::Value;

    Value integer(std::int32_t value)
    {
        return Value::ofInt(value);
    }

    Value text(const std::string& value)
    {
        return Value::ofVarchar(value);
    }

    ColumnRef bare(const std::string& name)
    {
        return ColumnRef{std::nullopt, name};
    }

    ColumnRef qualified(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    // R(a INT, b VARCHAR(1)) = {(1, x), (2, y), (3, x)}
    Schema schemaR()
    {
        return Schema({Column{"R", "a", Type::Int, 0}, Column{"R", "b", Type::Varchar, 1}});
    }

    TupleSourcePtr sourceR()
    {
        return std::make_unique<ra::VectorSource>(
            schemaR(), std::vector<Tuple>{{integer(1), text("x")}, {integer(2), text("y")}, {integer(3), text("x")}});
    }

    // S(c INT, d VARCHAR(1)) = {(1, p), (3, q)}
    TupleSourcePtr sourceS()
    {
        return std::make_unique<ra::VectorSource>(
            Schema({Column{"S", "c", Type::Int, 0}, Column{"S", "d", Type::Varchar, 1}}),
            std::vector<Tuple>{{integer(1), text("p")}, {integer(3), text("q")}});
    }

    // A one-column INT source `name(k)` holding `keys` in order (duplicates allowed).
    TupleSourcePtr keys(const std::string& name, const std::vector<std::int32_t>& values)
    {
        std::vector<Tuple> rows;
        for (std::int32_t value : values)
        {
            rows.push_back({integer(value)});
        }
        return std::make_unique<ra::VectorSource>(Schema({Column{name, "k", Type::Int, 0}}), std::move(rows));
    }

    db::Condition condition(const std::string& text)
    {
        return db::parseCondition(text);
    }

    // Every tuple as "v1 v2 ...", in output order.
    std::vector<std::string> drain(db::TupleSource& source)
    {
        std::vector<std::string> rows;
        Tuple tuple;
        while (source.next(tuple))
        {
            std::string row;
            for (const Value& value : tuple)
            {
                row += (row.empty() ? "" : " ") + value.toText();
            }
            rows.push_back(row);
        }
        return rows;
    }

    // "R.a INT, R.b VARCHAR" in schema order.
    std::string describe(const Schema& schema)
    {
        std::string out;
        for (const Column& column : schema.columns())
        {
            out += (out.empty() ? "" : ", ") + column.qualifiedName() + " " + std::string(db::typeName(column.type));
        }
        return out;
    }

    // Wraps a source and counts how it is used.
    class SpySource final : public db::TupleSource
    {
    public:
        SpySource(TupleSourcePtr inner, int& nextCalls, int& rewinds)
            : inner_(std::move(inner)), nextCalls_(nextCalls), rewinds_(rewinds)
        {
        }

        const Schema& schema() const override
        {
            return inner_->schema();
        }

        bool next(Tuple& out) override
        {
            ++nextCalls_;
            return inner_->next(out);
        }

        void rewind() override
        {
            ++rewinds_;
            inner_->rewind();
        }

    private:
        TupleSourcePtr inner_;
        int& nextCalls_;
        int& rewinds_;
    };

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

    using Rows = std::vector<std::string>;

} // namespace

TEST_CASE("select keeps matching tuples in order and counts every tuple examined")
{
    OperatorCounters counters;
    TupleSourcePtr source = db::select(sourceR(), condition("a > 1"), counters);
    CHECK(describe(source->schema()) == "R.a INT, R.b VARCHAR");
    CHECK(drain(*source) == Rows{"2 y", "3 x"});
    CHECK(counters.selectTuples == 3);
    CHECK(counters.joinComparisons == 0);
}

TEST_CASE("select with no match still counts every input tuple")
{
    OperatorCounters counters;
    TupleSourcePtr source = db::select(sourceR(), condition("R.a > 99"), counters);
    CHECK(drain(*source).empty());
    CHECK(counters.selectTuples == 3);
}

TEST_CASE("project keeps the listed columns in list order")
{
    TupleSourcePtr source = db::project(sourceR(), {bare("b"), qualified("R", "a")});
    CHECK(describe(source->schema()) == "R.b VARCHAR, R.a INT");
    CHECK(drain(*source) == Rows{"x 1", "y 2", "x 3"});
}

TEST_CASE("project keeps duplicate tuples (bags)")
{
    TupleSourcePtr source = db::project(sourceR(), {bare("b")});
    CHECK(drain(*source) == Rows{"x", "y", "x"});
}

TEST_CASE("distinct gives each tuple once, in first-occurrence order")
{
    CHECK(drain(*db::distinct(keys("L", {3, 1, 3, 2, 1, 3}))) == Rows{"3", "1", "2"});
    CHECK(drain(*db::distinct(db::project(sourceR(), {bare("b")}))) == Rows{"x", "y"});
    CHECK(drain(*db::distinct(sourceR())) == Rows{"1 x", "2 y", "3 x"});
}

TEST_CASE("distinct keeps the input schema, including qualifiers and VARCHAR lengths")
{
    const Schema input({Column{"T", "a", Type::Int, 0}, Column{"T", "b", Type::Varchar, 7}});
    TupleSourcePtr source = db::distinct(std::make_unique<ra::VectorSource>(
        input, std::vector<Tuple>{{integer(1), text("x")}, {integer(1), text("x")}}));
    REQUIRE(source->schema().size() == 2);
    CHECK(source->schema().column(0).qualifiedName() == "T.a");
    CHECK(source->schema().column(0).type == Type::Int);
    CHECK(source->schema().column(1).qualifiedName() == "T.b");
    CHECK(source->schema().column(1).type == Type::Varchar);
    CHECK(source->schema().column(1).varcharLength == 7);
    CHECK(drain(*source) == Rows{"1 x"});
}

TEST_CASE("distinct over an empty input gives no tuples")
{
    TupleSourcePtr source = db::distinct(keys("L", {}));
    CHECK(describe(source->schema()) == "L.k INT");
    CHECK(drain(*source).empty());
}

TEST_CASE("distinct rewind forgets the tuples already produced and rewinds its input")
{
    int reads = 0;
    int rewinds = 0;
    TupleSourcePtr source = db::distinct(std::make_unique<SpySource>(keys("L", {2, 2, 5}), reads, rewinds));
    CHECK(drain(*source) == Rows{"2", "5"});
    source->rewind();
    CHECK(rewinds == 1);
    // Without discarding its state, distinct would now drop 2 and 5 as repeats.
    CHECK(drain(*source) == Rows{"2", "5"});
}

TEST_CASE("set operations still remove duplicates when their inputs have them")
{
    // A = {1, 1, 2}, B = {2, 2, 3}
    CHECK(drain(*db::unionOf(keys("A", {1, 1, 2}), keys("B", {2, 2, 3}))) == Rows{"1", "2", "3"});
    CHECK(drain(*db::intersect(keys("A", {1, 1, 2}), keys("B", {2, 2, 3}))) == Rows{"2"});
    CHECK(drain(*db::except(keys("A", {1, 1, 2}), keys("B", {2, 2, 3}))) == Rows{"1"});
}

TEST_CASE("rename requalifies every column and leaves the tuples alone")
{
    TupleSourcePtr source = db::rename(sourceR(), "T");
    CHECK(describe(source->schema()) == "T.a INT, T.b VARCHAR");
    CHECK(drain(*source) == Rows{"1 x", "2 y", "3 x"});
}

TEST_CASE("cross pairs every left tuple with every right tuple and counts nothing")
{
    OperatorCounters counters;
    TupleSourcePtr source = db::cross(sourceR(), sourceS());
    CHECK(describe(source->schema()) == "R.a INT, R.b VARCHAR, S.c INT, S.d VARCHAR");
    CHECK(drain(*source) == Rows{"1 x 1 p", "1 x 3 q", "2 y 1 p", "2 y 3 q", "3 x 1 p", "3 x 3 q"});
    CHECK(counters.joinComparisons == 0);
    CHECK(counters.selectTuples == 0);
}

TEST_CASE("join keeps matching pairs and counts |left| x |right| comparisons")
{
    OperatorCounters counters;
    TupleSourcePtr source = db::join(sourceR(), sourceS(), condition("R.a = S.c"), counters);
    CHECK(describe(source->schema()) == "R.a INT, R.b VARCHAR, S.c INT, S.d VARCHAR");
    CHECK(drain(*source) == Rows{"1 x 1 p", "3 x 3 q"});
    CHECK(counters.joinComparisons == 6);
    CHECK(counters.selectTuples == 0);
}

TEST_CASE("join rewinds and rereads its right input once per left tuple")
{
    int leftReads = 0;
    int leftRewinds = 0;
    int rightReads = 0;
    int rightRewinds = 0;
    OperatorCounters counters;
    TupleSourcePtr source = db::join(std::make_unique<SpySource>(sourceR(), leftReads, leftRewinds),
                                     std::make_unique<SpySource>(sourceS(), rightReads, rightRewinds),
                                     condition("b = d"), counters);
    CHECK(drain(*source).empty());
    CHECK(rightRewinds == 3);
    CHECK(rightReads == 9); // 3 left tuples x (2 tuples + the end)
    CHECK(leftReads == 4);
    CHECK(leftRewinds == 0);
    CHECK(counters.joinComparisons == 6);
}

TEST_CASE("a select below a join's right input is counted once per left tuple")
{
    OperatorCounters counters;
    TupleSourcePtr right = db::select(sourceS(), condition("c > 0"), counters);
    TupleSourcePtr source = db::join(sourceR(), std::move(right), condition("a = c"), counters);
    CHECK(drain(*source) == Rows{"1 x 1 p", "3 x 3 q"});
    CHECK(counters.selectTuples == 6); // Bonus 1 counted 2: it read each subtree once
    CHECK(counters.joinComparisons == 6);
}

TEST_CASE("rewind of every operator produces the same output again, even after a partial read")
{
    OperatorCounters counters;
    std::vector<std::pair<std::string, TupleSourcePtr>> operators;
    operators.emplace_back("select", db::select(sourceR(), condition("b = 'x'"), counters));
    operators.emplace_back("project", db::project(sourceR(), {bare("b")}));
    operators.emplace_back("distinct", db::distinct(keys("L", {3, 1, 3, 2})));
    operators.emplace_back("rename", db::rename(sourceR(), "T"));
    operators.emplace_back("cross", db::cross(sourceR(), sourceS()));
    operators.emplace_back("join", db::join(sourceR(), sourceS(), condition("a = c"), counters));
    operators.emplace_back("union", db::unionOf(keys("L", {3, 1, 3, 2}), keys("M", {2, 4, 1, 4, 5})));
    operators.emplace_back("intersect", db::intersect(keys("L", {3, 1, 3, 2}), keys("M", {2, 4, 1, 4, 5})));
    operators.emplace_back("except", db::except(keys("L", {3, 1, 3, 2}), keys("M", {2, 4, 1, 4, 5})));
    for (auto& [name, source] : operators)
    {
        INFO("operator: ", name);
        const Rows first = drain(*source);
        REQUIRE_FALSE(first.empty());
        source->rewind();
        CHECK(drain(*source) == first);
        source->rewind();
        Tuple tuple;
        REQUIRE(source->next(tuple));
        source->rewind();
        CHECK(drain(*source) == first);
    }
}

TEST_CASE("next keeps returning false at the end until rewind")
{
    OperatorCounters counters;
    TupleSourcePtr source = db::join(sourceR(), sourceS(), condition("a = c"), counters);
    drain(*source);
    Tuple tuple;
    CHECK_FALSE(source->next(tuple));
    CHECK_FALSE(source->next(tuple));
    CHECK(counters.joinComparisons == 6);
}

TEST_CASE("union gives the left tuples then the new right tuples, each once, with the left schema")
{
    TupleSourcePtr source = db::unionOf(keys("L", {3, 1, 3, 2}), keys("M", {2, 4, 1, 4, 5}));
    CHECK(describe(source->schema()) == "L.k INT");
    CHECK(drain(*source) == Rows{"3", "1", "2", "4", "5"});
}

TEST_CASE("intersect gives the left tuples also in the right, each once, in left order")
{
    TupleSourcePtr source = db::intersect(keys("L", {3, 1, 3, 2}), keys("M", {2, 4, 1, 4, 5}));
    CHECK(describe(source->schema()) == "L.k INT");
    CHECK(drain(*source) == Rows{"1", "2"});
}

TEST_CASE("except gives the left tuples not in the right, each once, in left order")
{
    TupleSourcePtr source = db::except(keys("L", {3, 1, 3, 2, 6}), keys("M", {2, 4, 1, 4, 5}));
    CHECK(describe(source->schema()) == "L.k INT");
    CHECK(drain(*source) == Rows{"3", "6"});
}

TEST_CASE("set operations on empty inputs")
{
    CHECK(drain(*db::unionOf(keys("L", {}), keys("M", {1}))) == Rows{"1"});
    CHECK(drain(*db::intersect(keys("L", {1}), keys("M", {}))).empty());
    CHECK(drain(*db::except(keys("L", {1}), keys("M", {}))) == Rows{"1"});
}

TEST_CASE("every operator error is thrown at construction, before any tuple is read")
{
    int reads = 0;
    int rewinds = 0;
    const auto spyR = [&] { return std::make_unique<SpySource>(sourceR(), reads, rewinds); };
    const auto spyS = [&] { return std::make_unique<SpySource>(sourceS(), reads, rewinds); };
    OperatorCounters counters;

    struct Case
    {
        const char* what;
        std::optional<db::Error> error;
        ErrorCategory expected;
    };
    const Case cases[] = {
        {"unknown column", errorFrom([&] { db::select(spyR(), condition("z = 1"), counters); }),
         ErrorCategory::Name},
        {"ambiguous bare name",
         errorFrom([&] { db::select(db::cross(spyR(), db::rename(spyR(), "T")), condition("a = 1"), counters); }),
         ErrorCategory::Ambiguity},
        {"INT compared with VARCHAR", errorFrom([&] { db::select(spyR(), condition("a = 'x'"), counters); }),
         ErrorCategory::Type},
        {"join condition type", errorFrom([&] { db::join(spyR(), spyS(), condition("a = d"), counters); }),
         ErrorCategory::Type},
        {"join condition unknown column",
         errorFrom([&] { db::join(spyR(), spyS(), condition("R.a = S.z"), counters); }), ErrorCategory::Name},
        {"duplicate projection", errorFrom([&] { db::project(spyR(), {bare("a"), qualified("R", "a")}); }),
         ErrorCategory::Schema},
        {"projection of unknown column", errorFrom([&] { db::project(spyR(), {qualified("S", "a")}); }),
         ErrorCategory::Name},
        {"union column count", errorFrom([&] { db::unionOf(spyR(), keys("M", {1})); }), ErrorCategory::Schema},
        {"intersect column types",
         errorFrom([&] { db::intersect(spyR(), db::project(spyS(), {bare("d"), bare("c")})); }),
         ErrorCategory::Schema},
        {"except column count", errorFrom([&] { db::except(keys("M", {1}), spyS()); }), ErrorCategory::Schema},
        {"self join without rename", errorFrom([&] { db::join(spyR(), spyR(), condition("a = a"), counters); }),
         ErrorCategory::Schema},
        {"self cross without rename", errorFrom([&] { db::cross(spyR(), spyR()); }), ErrorCategory::Schema},
        {"rename to clashing names", errorFrom([&] { db::rename(db::cross(spyR(), db::rename(spyR(), "T")), "X"); }),
         ErrorCategory::Schema},
    };
    for (const Case& c : cases)
    {
        INFO("case: ", c.what);
        REQUIRE(c.error.has_value());
        CHECK(c.error->category() == c.expected);
    }
    CHECK(reads == 0);
    CHECK(counters.selectTuples == 0);
    CHECK(counters.joinComparisons == 0);
}

namespace
{

    // Opens R and S; any other name is unknown.
    TupleSourcePtr openTable(const std::string& name)
    {
        if (name == "R")
        {
            return sourceR();
        }
        if (name == "S")
        {
            return sourceS();
        }
        throw db::Error(ErrorCategory::Name, "Unknown relation '" + name + "'.");
    }

    Rows run(const std::string& query, OperatorCounters& counters)
    {
        TupleSourcePtr source = db::buildOperators(db::parseRaQuery(query), openTable, counters);
        return drain(*source);
    }

} // namespace

TEST_CASE("buildOperators runs every Phase 0 operator of a parsed query")
{
    OperatorCounters counters;
    CHECK(run("R", counters) == Rows{"1 x", "2 y", "3 x"});
    CHECK(run("select[a >= 2](R)", counters) == Rows{"2 y", "3 x"});
    CHECK(run("project[b](R)", counters) == Rows{"x", "y", "x"});
    CHECK(run("distinct(project[b](R))", counters) == Rows{"x", "y"});
    CHECK(run("rename[T](R) join[T.a = R.a] R", counters) == Rows{"1 x 1 x", "2 y 2 y", "3 x 3 x"});
    CHECK(run("R times S", counters).size() == 6);
    CHECK(run("project[a](R) union project[c](S)", counters) == Rows{"1", "2", "3"});
    CHECK(run("project[a](R) intersect project[c](S)", counters) == Rows{"1", "3"});
    CHECK(run("project[a](R) minus project[c](S)", counters) == Rows{"2"});
    CHECK(counters.selectTuples == 3);
    CHECK(counters.joinComparisons == 9);
}

TEST_CASE("buildOperators gives the schema outputSchema computes")
{
    OperatorCounters counters;
    const PlanNode plan = db::parseRaQuery("project[S.d, T.a](rename[T](R) join[T.a = S.c] S)");
    TupleSourcePtr source = db::buildOperators(plan, openTable, counters);
    CHECK(describe(source->schema()) == "S.d VARCHAR, T.a INT");
    CHECK(drain(*source) == Rows{"p 1", "q 3"});
}

TEST_CASE("buildOperators reports an unknown table as a name error from openTable")
{
    OperatorCounters counters;
    const auto error = errorFrom([&] { db::buildOperators(db::parseRaQuery("R union Nope"), openTable, counters); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message() == "Unknown relation 'Nope'.");
}

TEST_CASE("buildOperators finds errors children first, left before right")
{
    OperatorCounters counters;
    // The left select's Name error comes before the right side's unknown table
    // and before the union's Schema error.
    const auto error = errorFrom(
        [&] { db::buildOperators(db::parseRaQuery("select[z = 1](R) union (S times Nope)"), openTable, counters); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message() != "Unknown relation 'Nope'.");
}

TEST_CASE("buildOperators rejects malformed nodes as internal errors")
{
    OperatorCounters counters;
    PlanNode noDistinctChild = PlanNode::distinct(PlanNode::scan("R"));
    noDistinctChild.children.clear();
    CHECK_THROWS_AS(db::buildOperators(noDistinctChild, openTable, counters), std::logic_error);
    PlanNode noCondition = PlanNode::select(condition("a = 1"), PlanNode::scan("R"));
    noCondition.condition.reset();
    CHECK_THROWS_AS(db::buildOperators(noCondition, openTable, counters), std::logic_error);
    PlanNode noChild = PlanNode::rename("T", PlanNode::scan("R"));
    noChild.children.clear();
    CHECK_THROWS_AS(db::buildOperators(noChild, openTable, counters), std::logic_error);
    const auto nullTable = [](const std::string&) { return TupleSourcePtr(); };
    CHECK_THROWS_AS(db::buildOperators(PlanNode::scan("R"), nullTable, counters), std::logic_error);
}

TEST_CASE("a 1000-level plan of renames and selects builds and runs")
{
    PlanNode plan = PlanNode::scan("R");
    for (int level = 0; level < db::kMaxNestingDepth; ++level)
    {
        if (level % 2 == 0)
        {
            plan = PlanNode::rename("T" + std::to_string(level), std::move(plan));
        }
        else
        {
            plan = PlanNode::select(condition("a > 0"), std::move(plan));
        }
    }
    OperatorCounters counters;
    TupleSourcePtr source = db::buildOperators(plan, openTable, counters);
    CHECK(describe(source->schema()) == "T998.a INT, T998.b VARCHAR");
    CHECK(drain(*source) == Rows{"1 x", "2 y", "3 x"});
    CHECK(counters.selectTuples == 1500); // 500 selects x 3 tuples
}
