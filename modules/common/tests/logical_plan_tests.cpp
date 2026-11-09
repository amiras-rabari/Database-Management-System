// Logical group_by, sort, limit and union_all (sql chunk 06): builders,
// one-line text, output schema, JSON round trip, and the logical reader's
// errors. The plans are the spec's examples 5.4 and 5.7 (sql SPEC §M3.5).

#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace
{

    using db::Aggregate;
    using db::AggregateFunction;
    using db::Column;
    using db::ColumnRef;
    using db::ErrorCategory;
    using db::PlanNode;
    using db::Schema;
    using db::SortKey;
    using db::Type;

    // Emp(eid INT, name VARCHAR(20), age INT, did INT, salary INT,
    // city VARCHAR(10)), the table of the spec's examples; Dept(did INT,
    // city VARCHAR(10)) for example 5.7.
    std::optional<Schema> tables(const std::string& name)
    {
        if (name == "Emp")
        {
            return Schema({{"Emp", "eid", Type::Int, 0},
                           {"Emp", "name", Type::Varchar, 20},
                           {"Emp", "age", Type::Int, 0},
                           {"Emp", "did", Type::Int, 0},
                           {"Emp", "salary", Type::Int, 0},
                           {"Emp", "city", Type::Varchar, 10}});
        }
        if (name == "Dept")
        {
            return Schema({{"Dept", "did", Type::Int, 0}, {"Dept", "city", Type::Varchar, 10}});
        }
        return std::nullopt;
    }

    ColumnRef col(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    ColumnRef bare(const std::string& name)
    {
        return ColumnRef{std::nullopt, name};
    }

    // group_by keys ["Emp.did"] aggregates [["count","*","n"],["avg","Emp.salary","avg_sal"]]
    // over select "Emp.age > 25" over scan Emp (example 5.4, lower part).
    PlanNode example54GroupBy()
    {
        return PlanNode::groupBy({col("Emp", "did")},
                                 {Aggregate{AggregateFunction::Count, std::nullopt, "n"},
                                  Aggregate{AggregateFunction::Avg, col("Emp", "salary"), "avg_sal"}},
                                 PlanNode::select(db::parseCondition("Emp.age > 25"), PlanNode::scan("Emp")));
    }

    // The whole plan of example 5.4.
    PlanNode example54()
    {
        return PlanNode::limit(
            10, PlanNode::sort({SortKey{bare("n"), false}},
                               PlanNode::project({col("Emp", "did"), bare("n"), bare("avg_sal")},
                                                 PlanNode::select(db::parseCondition("n > 5"), example54GroupBy()))));
    }

    // Writes the plan as JSON text, reads it back with the logical reader,
    // and checks the tree is unchanged.
    void checkRoundTrip(const PlanNode& plan)
    {
        const std::string text = db::toJsonText(plan, -1);
        INFO("json: ", text);
        CHECK(db::fromJsonText(text) == plan);
    }

    // The error the logical reader gives for this JSON text.
    db::Error logicalReadError(const std::string& text)
    {
        try
        {
            db::fromJsonText(text);
        }
        catch (const db::Error& error)
        {
            return error;
        }
        FAIL("the logical reader accepted: " << text);
        return db::Error(ErrorCategory::Schema, "");
    }

    const std::string kScanEmp = R"({"op":"scan","table":"Emp"})";

} // namespace

TEST_CASE("logical plan: example 5.4 as JSON, field for field")
{
    CHECK(db::toJsonText(example54(), -1) ==
          R"({"op":"limit","n":10,"child":{"op":"sort","keys":[["n","desc"]],"child":)"
          R"({"op":"project","columns":["Emp.did","n","avg_sal"],"child":)"
          R"({"op":"select","condition":"n > 5","child":)"
          R"({"op":"group_by","keys":["Emp.did"],"aggregates":[["count","*","n"],["avg","Emp.salary","avg_sal"]],"child":)"
          R"({"op":"select","condition":"Emp.age > 25","child":{"op":"scan","table":"Emp"}}}}}}})");
    checkRoundTrip(example54());
}

TEST_CASE("logical plan: each new node round-trips")
{
    // group_by with no keys (SELECT COUNT(*) FROM Emp, required case 13) and
    // with no aggregates (GROUP BY without aggregates).
    checkRoundTrip(PlanNode::groupBy({}, {Aggregate{AggregateFunction::Count, std::nullopt, "count_star"}},
                                     PlanNode::scan("Emp")));
    checkRoundTrip(PlanNode::groupBy({col("Emp", "did"), col("Emp", "age")}, {}, PlanNode::scan("Emp")));
    checkRoundTrip(PlanNode::groupBy({},
                                     {Aggregate{AggregateFunction::Sum, col("Emp", "salary"), "s"},
                                      Aggregate{AggregateFunction::Min, col("Emp", "name"), "lo"},
                                      Aggregate{AggregateFunction::Max, bare("age"), "hi"}},
                                     PlanNode::scan("Emp")));
    checkRoundTrip(PlanNode::sort({SortKey{col("Emp", "age"), true}, SortKey{col("Emp", "name"), false}},
                                  PlanNode::scan("Emp")));
    checkRoundTrip(PlanNode::limit(0, PlanNode::scan("Emp")));
    checkRoundTrip(PlanNode::unionAll(PlanNode::scan("Emp"), PlanNode::scan("Dept")));
}

TEST_CASE("logical plan: the alias is part of the node")
{
    // Two group_bys that differ only in an alias are different plans.
    PlanNode renamed = example54GroupBy();
    renamed.aggregates[0].alias = "how_many";
    CHECK_FALSE(renamed == example54GroupBy());
}

TEST_CASE("logical plan: example 5.7, UNION ALL and UNION differ only in op")
{
    const PlanNode left = PlanNode::project({col("Emp", "city")}, PlanNode::scan("Emp"));
    const PlanNode right = PlanNode::project({col("Dept", "city")}, PlanNode::scan("Dept"));
    const PlanNode all = PlanNode::unionAll(left, right);
    const PlanNode distinct = PlanNode::unionOf(left, right);
    checkRoundTrip(all);

    std::string allText = db::toJsonText(all, -1);
    const std::string distinctText = db::toJsonText(distinct, -1);
    CHECK(allText.rfind(R"({"op":"union_all",)", 0) == 0);
    allText.replace(0, std::string(R"({"op":"union_all")").size(), R"({"op":"union")");
    CHECK(allText == distinctText);

    // Same schema rule as union: the left schema, after the compatibility check.
    const Schema schema = db::outputSchema(all, tables);
    REQUIRE(schema.size() == 1);
    CHECK(schema.column(0).qualifiedName() == "Emp.city");
}

TEST_CASE("logical plan: one-line text of the new nodes")
{
    CHECK(db::toString(example54()) ==
          "limit[10](sort[n desc](project[Emp.did, n, avg_sal](select[n > 5]("
          "group_by[Emp.did; count(*) as n, avg(Emp.salary) as avg_sal](select[Emp.age > 25](scan(Emp)))))))");
    CHECK(db::toString(PlanNode::groupBy({}, {}, PlanNode::scan("Emp"))) == "group_by[; ](scan(Emp))");
    CHECK(db::toString(PlanNode::unionAll(PlanNode::scan("Emp"), PlanNode::scan("Dept"))) ==
          "union_all(scan(Emp), scan(Dept))");
}

TEST_CASE("logical plan: output schema of group_by, sort and limit")
{
    // Example 5.4: keys as in the child, then one bare column per aggregate,
    // named by its alias. COUNT is INT, AVG is REAL.
    const Schema grouped = db::outputSchema(example54GroupBy(), tables);
    REQUIRE(grouped.size() == 3);
    CHECK(grouped.column(0).qualifiedName() == "Emp.did");
    CHECK(grouped.column(0).type == Type::Int);
    CHECK(grouped.column(1).qualifiedName() == "n");
    CHECK(grouped.column(1).type == Type::Int);
    CHECK(grouped.column(2).qualifiedName() == "avg_sal");
    CHECK(grouped.column(2).type == Type::Real);

    // SUM is INT; MIN and MAX keep the column's type and VARCHAR length.
    const Schema others = db::outputSchema(
        PlanNode::groupBy({},
                          {Aggregate{AggregateFunction::Sum, col("Emp", "salary"), "s"},
                           Aggregate{AggregateFunction::Min, col("Emp", "name"), "lo"},
                           Aggregate{AggregateFunction::Max, bare("age"), "hi"},
                           Aggregate{AggregateFunction::Count, col("Emp", "name"), "c"}},
                          PlanNode::scan("Emp")),
        tables);
    REQUIRE(others.size() == 4);
    CHECK(others.column(0).type == Type::Int);
    CHECK(others.column(1).type == Type::Varchar);
    CHECK(others.column(1).varcharLength == 20);
    CHECK(others.column(2).type == Type::Int);
    CHECK(others.column(3).type == Type::Int);

    // The whole example 5.4: sort and limit pass the child's schema through.
    const Schema whole = db::outputSchema(example54(), tables);
    REQUIRE(whole.size() == 3);
    CHECK(whole.column(1).qualifiedName() == "n");
    CHECK(whole.column(2).type == Type::Real);

    // Example 5.6: a rename above the group_by qualifies the aggregate (t.n).
    const Schema renamed = db::outputSchema(PlanNode::rename("t", example54GroupBy()), tables);
    CHECK(renamed.column(1).qualifiedName() == "t.n");
}

TEST_CASE("logical plan: output schema errors")
{
    // An unknown key, aggregate column or sort key: Error(Name).
    CHECK_THROWS_AS(db::outputSchema(PlanNode::groupBy({col("Emp", "nope")}, {}, PlanNode::scan("Emp")), tables),
                    db::Error);
    try
    {
        db::outputSchema(PlanNode::sort({SortKey{col("Emp", "nope"), true}}, PlanNode::scan("Emp")), tables);
        FAIL("unknown sort key accepted");
    }
    catch (const db::Error& error)
    {
        CHECK(error.category() == ErrorCategory::Name);
    }
    try
    {
        db::outputSchema(
            PlanNode::groupBy({}, {Aggregate{AggregateFunction::Max, col("Emp", "nope"), "m"}}, PlanNode::scan("Emp")),
            tables);
        FAIL("unknown aggregate column accepted");
    }
    catch (const db::Error& error)
    {
        CHECK(error.category() == ErrorCategory::Name);
    }

    // SUM and AVG need an INT column (sql §M3.3 type rules): Error(Type).
    try
    {
        db::outputSchema(
            PlanNode::groupBy({}, {Aggregate{AggregateFunction::Avg, col("Emp", "name"), "a"}}, PlanNode::scan("Emp")),
            tables);
        FAIL("AVG over VARCHAR accepted");
    }
    catch (const db::Error& error)
    {
        CHECK(error.category() == ErrorCategory::Type);
        CHECK(error.message() == "Aggregate 'avg' needs an INT column, but 'Emp.name' is VARCHAR(20).");
    }

    // Two aggregates with the same alias: Schema's duplicate rule.
    try
    {
        db::outputSchema(PlanNode::groupBy({},
                                           {Aggregate{AggregateFunction::Count, std::nullopt, "n"},
                                            Aggregate{AggregateFunction::Sum, col("Emp", "age"), "n"}},
                                           PlanNode::scan("Emp")),
                         tables);
        FAIL("duplicate alias accepted");
    }
    catch (const db::Error& error)
    {
        CHECK(error.category() == ErrorCategory::Schema);
    }

    // union_all of incompatible inputs: Error(Schema), as for union.
    try
    {
        db::outputSchema(PlanNode::unionAll(PlanNode::scan("Emp"), PlanNode::scan("Dept")), tables);
        FAIL("incompatible union_all accepted");
    }
    catch (const db::Error& error)
    {
        CHECK(error.category() == ErrorCategory::Schema);
    }
}

TEST_CASE("logical reader: rejects physical names and physical shapes")
{
    const db::Error hashJoin = logicalReadError(
        R"({"op":"hash_join","condition":"Emp.did = Dept.did","left":)" + kScanEmp + R"(,"right":)" + kScanEmp + "}");
    CHECK(hashJoin.message() == "Unknown plan operator 'hash_join'.");

    // A 2-element (physical) aggregate.
    const db::Error twoElements =
        logicalReadError(R"({"op":"group_by","keys":[],"aggregates":[["count","*"]],"child":)" + kScanEmp + "}");
    CHECK(twoElements.category() == ErrorCategory::Schema);
    CHECK(twoElements.message() == "Node 'group_by': each entry of 'aggregates' must be [function, column, alias].");

    // mode belongs to physical group_by only.
    const db::Error withMode = logicalReadError(R"({"op":"group_by","mode":"hash","keys":[],"aggregates":[],"child":)" +
                                                kScanEmp + "}");
    CHECK(withMode.message() == "Node 'group_by': unexpected field 'mode'.");

    // The alias must be a bare name, so a bare reference above can find it.
    const db::Error qualifiedAlias = logicalReadError(
        R"({"op":"group_by","keys":[],"aggregates":[["count","*","t.n"]],"child":)" + kScanEmp + "}");
    CHECK(qualifiedAlias.message() == "Node 'group_by': invalid alias 't.n'.");

    const db::Error down = logicalReadError(R"({"op":"sort","keys":[["Emp.age","down"]],"child":)" + kScanEmp + "}");
    CHECK(down.category() == ErrorCategory::Schema);
    CHECK(down.message() == "Node 'sort': each entry of 'keys' must be [column, \"asc\" or \"desc\"].");

    const db::Error noCount = logicalReadError(R"({"op":"limit","child":)" + kScanEmp + "}");
    CHECK(noCount.message() == "Node 'limit': missing required field 'n'.");
}

TEST_CASE("physical reader: still rejects the logical 3-element aggregate")
{
    try
    {
        db::physicalFromJsonText(R"({"op":"group_by","mode":"hash","keys":[],"aggregates":[["count","*","n"]],"child":)"
                                 R"({"op":"seqscan","table":"Emp"}})");
        FAIL("the physical reader accepted a 3-element aggregate");
    }
    catch (const db::Error& error)
    {
        CHECK(error.message() == "Node 'group_by': each entry of 'aggregates' must be [function, column].");
    }
}
