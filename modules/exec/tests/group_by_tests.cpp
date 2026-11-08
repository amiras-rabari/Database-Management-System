// CHUNK 10 tests: group_by in sort and hash mode (SPEC §M2.3). Decisions
// tested here (DECISIONS.md 2026-11-07): REAL for AVG with four decimals,
// SUM that does not fit in INT is Error(Type), generated output names with
// no qualifier, the empty-input edge cases.
//
// Table G(k INT, v INT, s VARCHAR(10)) with 3-byte s: a record is
// 4 + 4 + (2 + 3) = 13 bytes, so 240 rows fill a page and 7200 rows are 30
// pages.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/exec_context.h"
#include "db/exec/executable_plan.h"
#include "tests/test_db.h"

using namespace db;
using namespace db::exec;
using db::exec::test::TestDb;

namespace
{
    // E(dept VARCHAR(5), salary INT, name VARCHAR(10)): the spec's example.
    Schema eSchema()
    {
        return Schema({{"E", "dept", Type::Varchar, 5}, {"E", "salary", Type::Int, 0}, {"E", "name", Type::Varchar, 10}});
    }

    Tuple eRow(const std::string& dept, int salary, const std::string& name)
    {
        return {Value::ofVarchar(dept), Value::ofInt(salary), Value::ofVarchar(name)};
    }

    std::string groupBy(const std::string& mode, const std::string& keys, const std::string& aggregates,
                        const std::string& table)
    {
        return R"({"op": "group_by", "mode": ")" + mode + R"(", "keys": )" + keys + R"(, "aggregates": )" +
               aggregates + R"(, "child": {"op": "seqscan", "table": ")" + table + R"("}})";
    }

    // What one run of a plan gave.
    struct Result
    {
        Schema schema;
        std::vector<std::string> rows; // each row as "v1|v2|...", sorted (bags)
        std::size_t maxFrames = 0;
    };

    std::string rowText(const Tuple& row)
    {
        std::string text;
        for (std::size_t i = 0; i < row.size(); ++i)
            text += (i == 0 ? "" : "|") + row[i].toText();
        return text;
    }

    Result runPlan(TestDb& db, const std::string& json)
    {
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(json), ctx);
        Result result{plan.schema(), {}, 0};
        plan.open();
        Tuple t;
        while (plan.next(t))
            result.rows.push_back(rowText(t));
        plan.close();
        result.maxFrames = plan.maxFramesHeld();
        std::sort(result.rows.begin(), result.rows.end());
        return result;
    }

    // The db::Error that `function` throws, or nullopt.
    template <typename Function>
    std::optional<Error> errorFrom(Function function)
    {
        try
        {
            function();
        }
        catch (const Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    Schema gSchema()
    {
        return Schema({{"G", "k", Type::Int, 0}, {"G", "v", Type::Int, 0}, {"G", "s", Type::Varchar, 10}});
    }

    // 7200 rows (30 pages): k = (i * 7) mod groups, v = i mod 100, s = three
    // letters that vary with i, so MIN and MAX of s change inside a group.
    std::vector<Tuple> gRows(int groups)
    {
        std::vector<Tuple> rows;
        for (int i = 0; i < 7200; ++i)
        {
            const std::string s = {static_cast<char>('a' + i % 26), static_cast<char>('A' + i % 11), 'x'};
            rows.push_back({Value::ofInt((i * 7) % groups), Value::ofInt(i % 100), Value::ofVarchar(s)});
        }
        return rows;
    }

    // The expected rows of the large plans, worked out with a std::map.
    std::vector<std::string> expectedGroups(const std::vector<Tuple>& rows)
    {
        struct Group
        {
            int count = 0;
            long long sum = 0;
            std::string min, max;
        };
        std::map<int, Group> groups;
        for (const Tuple& row : rows)
        {
            Group& g = groups[row[0].asInt()];
            const std::string& s = row[2].asVarchar();
            if (g.count == 0 || s < g.min)
                g.min = s;
            if (g.count == 0 || s > g.max)
                g.max = s;
            ++g.count;
            g.sum += row[1].asInt();
        }
        std::vector<std::string> expected;
        for (const auto& [k, g] : groups)
        {
            expected.push_back(rowText({Value::ofInt(k), Value::ofInt(g.count), Value::ofInt(static_cast<int>(g.sum)),
                                        Value::ofReal(static_cast<double>(g.sum) / g.count), Value::ofVarchar(g.min),
                                        Value::ofVarchar(g.max)}));
        }
        std::sort(expected.begin(), expected.end());
        return expected;
    }

    const char* kAllAggregates =
        R"([["count","*"],["sum","G.v"],["avg","G.v"],["min","G.s"],["max","G.s"]])";
} // namespace

TEST_CASE("group_by: the spec's plan gives the spec's schema and the same bag in both modes")
{
    TestDb db("group_by_spec");
    db.createTable("E", eSchema(), {eRow("cs", 100, "ann"), eRow("cs", 200, "bob"), eRow("ee", 50, "cy")});
    for (const char* mode : {"sort", "hash"})
    {
        CAPTURE(mode);
        const Result r =
            runPlan(db, groupBy(mode, R"(["E.dept"])", R"([["count","*"],["avg","E.salary"],["max","E.name"]])", "E"));
        REQUIRE(r.schema.size() == 4);
        CHECK(r.schema.column(0).qualifiedName() == "E.dept");
        CHECK(r.schema.column(1).qualifiedName() == "count_star");
        CHECK(r.schema.column(2).qualifiedName() == "avg_E_salary");
        CHECK(r.schema.column(3).qualifiedName() == "max_E_name");
        CHECK(r.schema.column(0).type == Type::Varchar);
        CHECK(r.schema.column(0).varcharLength == 5);
        CHECK(r.schema.column(1).type == Type::Int);
        CHECK(r.schema.column(2).type == Type::Real);
        CHECK(r.schema.column(3).type == Type::Varchar);
        CHECK(r.schema.column(3).varcharLength == 10);
        CHECK(r.rows == std::vector<std::string>{"cs|2|150.0000|bob", "ee|1|50.0000|cy"});

        // No keys: one group over the whole input; sum, min and AVG of 350 / 3.
        const Result all = runPlan(
            db, groupBy(mode, "[]", R"([["count","E.name"],["sum","E.salary"],["avg","E.salary"],["min","E.salary"]])",
                        "E"));
        CHECK(all.rows == std::vector<std::string>{"3|350|116.6667|50"});
    }
}

TEST_CASE("group_by: no keys + empty input gives exactly one row with COUNT 0 (sql case 13)")
{
    TestDb db("group_by_empty_nokeys");
    db.createTable("E", eSchema(), {});
    for (const char* mode : {"sort", "hash"})
    {
        CAPTURE(mode);
        const Result r = runPlan(db, groupBy(mode, "[]", R"([["count","*"],["count","E.name"]])", "E"));
        CHECK(r.rows == std::vector<std::string>{"0|0"});
    }
}

TEST_CASE("group_by: no keys + empty input with avg, sum, min or max is an aggregate error")
{
    TestDb db("group_by_empty_avg");
    db.createTable("E", eSchema(), {});
    for (const char* mode : {"sort", "hash"})
    {
        CAPTURE(mode);
        const auto error =
            errorFrom([&] { runPlan(db, groupBy(mode, "[]", R"([["count","*"],["avg","E.salary"]])", "E")); });
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Aggregate);
        CHECK(std::string(error->what()).find("avg_E_salary") != std::string::npos);
        CHECK(db.engine().liveTempFileCount() == 0);
    }
}

TEST_CASE("group_by: keys + empty input gives zero rows")
{
    TestDb db("group_by_empty_keys");
    db.createTable("E", eSchema(), {});
    for (const char* mode : {"sort", "hash"})
    {
        CAPTURE(mode);
        const Result r = runPlan(db, groupBy(mode, R"(["E.dept"])", R"([["avg","E.salary"]])", "E"));
        CHECK(r.rows.empty());
    }
}

TEST_CASE("group_by: sum or avg of a VARCHAR is a type error while the plan is built, before any read")
{
    TestDb db("group_by_sum_varchar");
    db.createTable("E", eSchema(), {eRow("cs", 100, "ann")});
    db.coldPool();
    for (const char* fn : {"sum", "avg"})
    {
        CAPTURE(fn);
        ExecContext ctx(db.engine());
        const std::string json = groupBy("sort", R"(["E.dept"])", std::string(R"([[")") + fn + R"(","E.name"]])", "E");
        const auto error = errorFrom([&] { buildExecutablePlan(physicalFromJsonText(json), ctx); });
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Type);
        CHECK(db.engine().pool().stats().reads == 0);
    }
    // Unknown columns are rejected the same way (Name).
    ExecContext ctx(db.engine());
    const auto unknown = errorFrom([&] {
        buildExecutablePlan(physicalFromJsonText(groupBy("hash", R"(["E.zz"])", R"([["count","*"]])", "E")), ctx);
    });
    REQUIRE(unknown.has_value());
    CHECK(unknown->category() == ErrorCategory::Name);
}

TEST_CASE("group_by: a SUM that does not fit in INT is a type error naming the column")
{
    TestDb db("group_by_sum_overflow");
    db.createTable("E", eSchema(),
                   {eRow("cs", 1000000000, "a"), eRow("cs", 1000000000, "b"), eRow("cs", 1000000000, "c")});
    for (const char* mode : {"sort", "hash"})
    {
        CAPTURE(mode);
        const auto error = errorFrom([&] { runPlan(db, groupBy(mode, R"(["E.dept"])", R"([["sum","E.salary"]])", "E")); });
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Type);
        CHECK(std::string(error->what()).find("sum_E_salary = 3000000000 does not fit in INT") != std::string::npos);
    }
    // Two of them fit (2000000000 <= 2147483647).
    TestDb fits("group_by_sum_fits");
    fits.createTable("E", eSchema(), {eRow("cs", 1000000000, "a"), eRow("cs", 1000000000, "b")});
    CHECK(runPlan(fits, groupBy("hash", R"(["E.dept"])", R"([["sum","E.salary"]])", "E")).rows ==
          std::vector<std::string>{"cs|2000000000"});
}

TEST_CASE("group_by: MIN and MAX of a VARCHAR use byte order")
{
    TestDb db("group_by_minmax_bytes");
    db.createTable("E", eSchema(), {eRow("cs", 1, "adam"), eRow("cs", 2, "Zed"), eRow("cs", 3, "bea")});
    for (const char* mode : {"sort", "hash"})
    {
        CAPTURE(mode);
        // 'Z' is byte 90, 'a' is 97: "Zed" < "adam".
        const Result r = runPlan(db, groupBy(mode, R"(["E.dept"])", R"([["min","E.name"],["max","E.name"]])", "E"));
        CHECK(r.rows == std::vector<std::string>{"cs|Zed|bea"});
    }
}

TEST_CASE("group_by: 30 pages at B = 4, both modes equal the expected groups within 4 frames")
{
    // 1000 groups: the hash table (3 pages) overflows, so hash mode spills
    // and partitions. 5000 groups: a partition also has more groups than its
    // table, so that partition is sorted (the fallback).
    for (int groups : {1000, 5000})
    {
        CAPTURE(groups);
        TestDb db("group_by_large_" + std::to_string(groups), 4);
        const std::vector<Tuple> rows = gRows(groups);
        db.createTable("G", gSchema(), rows);
        const std::vector<std::string> expected = expectedGroups(rows);
        REQUIRE(expected.size() == static_cast<std::size_t>(groups));
        for (const char* mode : {"sort", "hash"})
        {
            CAPTURE(mode);
            const Result r = runPlan(db, groupBy(mode, R"(["G.k"])", kAllAggregates, "G"));
            CHECK(r.rows == expected);
            CHECK(r.maxFrames <= 4);
            CHECK(db.engine().liveTempFileCount() == 0);
        }
    }
}

TEST_CASE("group_by: a sort above it spills REAL averages to runs and orders them")
{
    // 5000 groups of (k, avg): 5000 * (4 + 8 + 4 slot) bytes = 20 pages, more
    // than the sort's 3-page runs at B = 4, so the REAL column goes to disk.
    TestDb db("group_by_sort_avg", 4);
    db.createTable("G", gSchema(), gRows(5000));
    const std::string json = R"({"op": "sort", "keys": [["avg_G_v", "asc"], ["G.k", "asc"]], "child": )" +
                             groupBy("hash", R"(["G.k"])", R"([["avg","G.v"]])", "G") + "}";
    ExecContext ctx(db.engine());
    ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(json), ctx);
    plan.open();
    std::vector<Tuple> rows;
    Tuple t;
    while (plan.next(t))
        rows.push_back(t);
    plan.close();
    REQUIRE(rows.size() == 5000);
    for (std::size_t i = 1; i < rows.size(); ++i)
        CHECK(rows[i - 1][1].asReal() <= rows[i][1].asReal());
    CHECK(plan.trace()[0].counters.pageWrites > 0); // the sort wrote runs
    CHECK(plan.maxFramesHeld() <= 4);
}
