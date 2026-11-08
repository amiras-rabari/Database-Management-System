// CHUNK 15: the SPEC §M2.8 checklist, one TEST_CASE per item, over a fixed
// catalogue of plans that covers every operator in several combinations.
// When this file is green, the checklist is met; use it in the oral check.
// (bench/exec_acceptance.py checks the same items again through the `exec`
// program; see modules/exec/SPEC_AUDIT.md.)
//
// Data: R(a INT, b VARCHAR(10), c INT) and S of the same shape (rRows: a = i,
// b = 'csc'/'eng', c = i mod 7; 240 rows per page), and K-tables (a INT, b
// INT) whose b values come from fixed formulas. No random numbers, so every
// run sees the same data and counters can be compared exactly.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
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
using db::exec::test::rRows;
using db::exec::test::rSchema;
using db::exec::test::TestDb;

namespace
{
    // ---------------------------------------------------------------- plans

    struct NamedPlan
    {
        std::string name;
        std::string json;
        /// What the plan gives when R and S are both empty (checklist item 7).
        std::size_t rowsWhenEmpty;
    };

    const char* const kR = R"({"op": "seqscan", "table": "R"})";
    const char* const kS = R"({"op": "seqscan", "table": "S"})";

    std::string join(const std::string& op, const std::string& condition, const std::string& left,
                     const std::string& right)
    {
        return R"({"op": ")" + op + R"(", "condition": ")" + condition + R"(", "left": )" + left +
               R"(, "right": )" + right + "}";
    }

    std::string project(const std::string& columns, const std::string& child)
    {
        return R"({"op": "project", "columns": )" + columns + R"(, "child": )" + child + "}";
    }

    // Every operator over a scan, both modes of distinct and group_by, the
    // three joins, joins under other operators, set operations of joins, a
    // self-join through rename, and the spec's own examples.
    std::vector<NamedPlan> catalogue()
    {
        const std::string hj = join("hash_join", "R.a = S.a", kR, kS);
        const std::string smj = join("sort_merge_join", "R.a = S.a", kR, kS);
        const std::string nlj = join("nested_loop_join", "R.a < S.c", kR, kS);
        const std::string twoColumns = R"(["R.a", "S.c"])";
        return {
            {"seqscan", kR, 0},
            {"filter", R"({"op": "filter", "condition": "R.a > 500 and R.b = 'csc'", "child": )" + std::string(kR) + "}", 0},
            {"project", project(R"(["R.c", "R.b"])", kR), 0},
            {"sort (spec example keys)", R"({"op": "sort", "keys": [["R.c", "asc"], ["R.b", "desc"]], "child": )" + std::string(kR) + "}", 0},
            {"distinct sort", R"({"op": "distinct", "mode": "sort", "child": )" + project(R"(["R.c"])", kR) + "}", 0},
            {"distinct hash", R"({"op": "distinct", "mode": "hash", "child": )" + project(R"(["R.c"])", kR) + "}", 0},
            {"group_by sort", R"({"op": "group_by", "mode": "sort", "keys": ["R.b"], "aggregates": [["count", "*"], ["avg", "R.a"], ["max", "R.b"]], "child": )" + std::string(kR) + "}", 0},
            {"group_by hash", R"({"op": "group_by", "mode": "hash", "keys": ["R.b"], "aggregates": [["count", "*"], ["sum", "R.c"], ["min", "R.a"]], "child": )" + std::string(kR) + "}", 0},
            {"group_by no keys", R"({"op": "group_by", "mode": "hash", "keys": [], "aggregates": [["count", "*"]], "child": )" + std::string(kR) + "}", 1},
            {"limit over sort", R"({"op": "limit", "n": 10, "child": {"op": "sort", "keys": [["R.a", "desc"]], "child": )" + std::string(kR) + "}}", 0},
            {"nested_loop_join (inequality)", nlj, 0},
            {"sort_merge_join", smj, 0},
            {"hash_join", hj, 0},
            {"spec §M2.2 example: hash_join over a filter",
             join("hash_join", "R.b = S.b", R"({"op": "filter", "condition": "R.a > 500", "child": )" + std::string(kR) + "}", kS), 0},
            {"filter and project above a join",
             project(twoColumns, R"({"op": "filter", "condition": "S.c > 2", "child": )" + hj + "}"), 0},
            {"sort above a hash join", R"({"op": "sort", "keys": [["S.c", "desc"]], "child": )" + hj + "}", 0},
            {"group_by above a sort-merge join", R"({"op": "group_by", "mode": "sort", "keys": ["S.c"], "aggregates": [["count", "*"]], "child": )" + smj + "}", 0},
            {"union of two joins", R"({"op": "union", "left": )" + project(twoColumns, hj) + R"(, "right": )" + project(twoColumns, smj) + "}", 0},
            {"intersect of two joins", R"({"op": "intersect", "left": )" + project(twoColumns, hj) + R"(, "right": )" + project(twoColumns, smj) + "}", 0},
            {"except of two joins", R"({"op": "except", "left": )" + project(twoColumns, hj) + R"(, "right": )" + project(twoColumns, nlj) + "}", 0},
            {"union_all", R"({"op": "union_all", "left": )" + std::string(kR) + R"(, "right": )" + std::string(kS) + "}", 0},
            {"self-join through rename",
             join("hash_join", "R.a = E.c", kR, R"({"op": "rename", "to": "E", "child": )" + std::string(kR) + "}"), 0},
            {"three-table nested loop chain",
             join("nested_loop_join", "S.c < E.c", nlj, R"({"op": "rename", "to": "E", "child": )" + std::string(kR) + "}"), 0},
        };
    }

    // ---------------------------------------------------------------- running

    struct Run
    {
        std::vector<std::string> rows; // "v1|v2|...", sorted (a bag)
        std::vector<TraceRow> trace;
        std::size_t maxFrames = 0;
    };

    std::string rowText(const Tuple& row)
    {
        std::string text;
        for (std::size_t i = 0; i < row.size(); ++i)
            text += (i == 0 ? "" : "|") + row[i].toText();
        return text;
    }

    Run run(TestDb& db, const std::string& json)
    {
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(json), ctx);
        Run result;
        plan.open();
        Tuple t;
        while (plan.next(t))
            result.rows.push_back(rowText(t));
        plan.close();
        std::sort(result.rows.begin(), result.rows.end());
        result.trace = plan.trace();
        result.maxFrames = plan.maxFramesHeld();
        return result;
    }

    // The checklist's "nothing left": no pinned frame, no temporary file.
    void checkNothingLeft(TestDb& db)
    {
        CHECK(db.engine().pool().pinnedFrameCount() == 0);
        CHECK(db.engine().liveTempFileCount() == 0);
    }

    // R (`rRows` rows) and S (`sRows` rows), both of rSchema's shape.
    void makeRS(TestDb& db, int rRowCount, int sRowCount)
    {
        db.createTable("R", rSchema("R"), rRows(rRowCount));
        db.createTable("S", rSchema("S"), rRows(sRowCount));
    }

    // K(a INT, b INT) with a = row number and b = `bs`.
    Schema kSchema(const std::string& table)
    {
        return Schema({{table, "a", Type::Int, 0}, {table, "b", Type::Int, 0}});
    }

    std::vector<Tuple> kRows(const std::vector<int>& bs)
    {
        std::vector<Tuple> rows;
        for (std::size_t i = 0; i < bs.size(); ++i)
            rows.push_back({Value::ofInt(static_cast<int>(i)), Value::ofInt(bs[i])});
        return rows;
    }

    std::optional<Error> buildError(TestDb& db, const std::string& json)
    {
        ExecContext ctx(db.engine());
        try
        {
            buildExecutablePlan(physicalFromJsonText(json), ctx);
        }
        catch (const Error& error)
        {
            return error;
        }
        return std::nullopt;
    }
} // namespace

TEST_CASE("checklist: all three joins on the same inputs return the same bag of tuples")
{
    // Datasets for R.b = S.b: uniform, many duplicates on both sides, one key
    // in 40 % of the rows (skew), and one side empty.
    struct Dataset
    {
        std::string name;
        std::vector<int> r, s;
    };
    std::vector<Dataset> datasets(4);
    datasets[0].name = "uniform";
    datasets[1].name = "duplicates on both sides";
    datasets[2].name = "skew";
    datasets[3].name = "right side empty";
    for (int i = 0; i < 1500; ++i)
    {
        datasets[0].r.push_back((i * 7919) % 3000);
        datasets[1].r.push_back(i % 25);
        datasets[2].r.push_back(i < 600 ? 7 : 1000 + i);
        datasets[3].r.push_back(i);
    }
    for (int i = 0; i < 700; ++i)
    {
        datasets[0].s.push_back((i * 104729) % 3000);
        datasets[1].s.push_back(i % 25);
        datasets[2].s.push_back(i < 280 ? 7 : 1000 + 2 * i);
    }
    for (const Dataset& data : datasets)
    {
        for (std::size_t frames : {4, 8, 32})
        {
            CAPTURE(data.name);
            CAPTURE(frames);
            TestDb db("checklist_same_bag", frames);
            db.createTable("R", kSchema("R"), kRows(data.r));
            db.createTable("S", kSchema("S"), kRows(data.s));
            const Run loop = run(db, join("nested_loop_join", "R.b = S.b", kR, kS));
            const Run merge = run(db, join("sort_merge_join", "R.b = S.b", kR, kS));
            const Run hash = run(db, join("hash_join", "R.b = S.b", kR, kS));
            CHECK(merge.rows == loop.rows);
            CHECK(hash.rows == loop.rows);
            checkNothingLeft(db);
        }
    }
}

TEST_CASE("checklist: every plan runs at B = 4")
{
    TestDb db("checklist_b4", 4);
    makeRS(db, 3000, 1000); // 13 and 5 pages
    for (const NamedPlan& plan : catalogue())
    {
        CAPTURE(plan.name);
        const Run result = run(db, plan.json);
        CHECK(result.maxFrames <= 4);
        CHECK(db.engine().pool().peakPinnedCount() <= 4);
        checkNothingLeft(db);
    }
}

TEST_CASE("checklist: a join whose output is larger than both inputs completes")
{
    // 200 rows per side, 10 key values, each 20 times per side: every key
    // gives 20 x 20 pairs, 10 keys x 400 = 4000 rows from inputs of 200.
    std::vector<int> keys;
    for (int i = 0; i < 200; ++i)
        keys.push_back(i % 10);
    TestDb db("checklist_large_output", 4);
    db.createTable("R", kSchema("R"), kRows(keys));
    db.createTable("S", kSchema("S"), kRows(keys));
    for (const std::string op : {"nested_loop_join", "sort_merge_join", "hash_join"})
    {
        CAPTURE(op);
        CHECK(run(db, join(op, "R.b = S.b", kR, kS)).rows.size() == 4000);
        checkNothingLeft(db);
    }
}

TEST_CASE("checklist: an empty input through every operator returns cleanly, including group_by")
{
    TestDb db("checklist_empty", 4);
    makeRS(db, 0, 0);
    for (const NamedPlan& plan : catalogue())
    {
        CAPTURE(plan.name);
        const Run result = run(db, plan.json);
        CHECK(result.rows.size() == plan.rowsWhenEmpty);
        checkNothingLeft(db);
    }
    // group_by with no keys on an empty input: one row, COUNT 0 (§M2.3).
    CHECK(run(db, catalogue()[8].json).rows == std::vector<std::string>{"0"});
}

TEST_CASE("checklist: after any plan, no pinned frames and no temporary files")
{
    TestDb db("checklist_nothing_left", 4);
    makeRS(db, 3000, 1000);
    db.createTable("E", rSchema("E"), {});
    // Every catalogue plan (checked inside the loop of "every plan runs at
    // B = 4" too), a plan stopped early by limit, and a plan that fails while
    // running (AVG of an empty input with no keys, §M2.3).
    run(db, R"({"op": "limit", "n": 2, "child": )" + join("hash_join", "R.a = S.a", kR, kS) + "}");
    checkNothingLeft(db);
    const std::string failing =
        R"({"op": "group_by", "mode": "sort", "keys": [], "aggregates": [["avg", "E.a"]], "child": {"op": "seqscan", "table": "E"}})";
    bool failed = false;
    try
    {
        run(db, failing);
    }
    catch (const Error&)
    {
        failed = true;
    }
    CHECK(failed);
    checkNothingLeft(db);
}

TEST_CASE("checklist: plan validation errors name the offending node; unknown names are rejected at open")
{
    TestDb db("checklist_validation", 4);
    makeRS(db, 2400, 10);
    db.coldPool();
    struct Bad
    {
        std::string json, message;
    };
    for (const Bad& bad : {Bad{R"({"op": "sort", "child": {"op": "seqscan", "table": "R"}})",
                               "Node 'sort': missing required field 'keys'."},
                           Bad{R"({"op": "filter", "condition": 5, "child": {"op": "seqscan", "table": "R"}})",
                               "Node 'filter': field 'condition' must be a string."},
                           Bad{R"({"op": "merge_join", "condition": "R.a = S.a", "left": {"op": "seqscan", "table": "R"}, "right": {"op": "seqscan", "table": "S"}})",
                               "merge_join"}})
    {
        CAPTURE(bad.json);
        const std::optional<Error> error = buildError(db, bad.json);
        REQUIRE(error.has_value());
        CHECK(std::string(error->what()).find(bad.message) != std::string::npos);
    }
    // An unknown column above a sort: rejected before the sort reads anything.
    const std::optional<Error> unknown = buildError(
        db, R"({"op": "filter", "condition": "R.zz > 1", "child": {"op": "sort", "keys": [["R.a", "asc"]], "child": {"op": "seqscan", "table": "R"}}})");
    REQUIRE(unknown.has_value());
    CHECK(unknown->category() == ErrorCategory::Name);
    CHECK(db.engine().pool().stats().reads == 0);
    CHECK(db.engine().liveTempFileCount() == 0);
    const std::optional<Error> table = buildError(db, R"({"op": "seqscan", "table": "Nope"})");
    REQUIRE(table.has_value());
    CHECK(table->category() == ErrorCategory::Name);
}

TEST_CASE("checklist: counters identical across repeated runs")
{
    // Each catalogue plan twice, each time on a fresh, cold pool. Small tables
    // (2 and 1 pages) are enough: the point is that the same plan, data and B
    // give the same counters, and these still make sorts write runs and hash
    // joins partition at B = 4. (The large tables of "every plan runs at
    // B = 4" made this case take over a minute, past ctest's 60 s limit.)
    TestDb db("checklist_repeat", 4);
    makeRS(db, 300, 100);
    for (const NamedPlan& plan : catalogue())
    {
        CAPTURE(plan.name);
        db.coldPool();
        const Run first = run(db, plan.json);
        db.coldPool();
        const Run again = run(db, plan.json);
        CHECK(again.trace == first.trace);
        CHECK(again.rows == first.rows);
    }
}

TEST_CASE("checklist: the spec's golden examples at plan level")
{
    TestDb db("checklist_golden", 4);
    // The duplicate-key example (§M2.3): R.b = [1, 2, 2, 3], S.b = [2, 2, 4] -> 4 rows.
    db.createTable("R", kSchema("R"), kRows({1, 2, 2, 3}));
    db.createTable("S", kSchema("S"), kRows({2, 2, 4}));
    for (const std::string op : {"sort_merge_join", "hash_join", "nested_loop_join"})
    {
        CAPTURE(op);
        CHECK(run(db, join(op, "R.b = S.b", kR, kS)).rows.size() == 4);
    }
    // Nested loop compares every pair: exactly |R| x |S| = 12.
    CHECK(run(db, join("nested_loop_join", "R.b = S.b", kR, kS)).trace[0].counters.comparisons == 12);

    // The sort example as it runs in a plan at B = 4: 8 pages, runs of 3
    // pages (the scan holds the 4th frame) -> 3 runs, 2 passes; the sort
    // writes the 8 pages once and reads them once (rewrite variant).
    db.createTable("P", rSchema("P"), rRows(1920));
    db.coldPool();
    const Run sort = run(db, R"({"op": "sort", "keys": [["P.a", "desc"]], "child": {"op": "seqscan", "table": "P"}})");
    CHECK(sort.trace[0].details["initial_runs"] == 3);
    CHECK(sort.trace[0].details["passes"] == 2);
    CHECK(sort.trace[0].counters.pageWrites == 8);
    CHECK(sort.trace[0].counters.pageReads == 8);
}

TEST_CASE("checklist: a sort of 20 x B pages at B = 4 completes within 4 frames (measurement 5)")
{
    TestDb db("checklist_20b", 4);
    db.createTable("P", rSchema("P"), rRows(80 * 240));
    const Run sort = run(db, R"({"op": "sort", "keys": [["P.a", "desc"]], "child": {"op": "seqscan", "table": "P"}})");
    CHECK(sort.rows.size() == 80 * 240);
    CHECK(sort.maxFrames <= 4);
    checkNothingLeft(db);
}
