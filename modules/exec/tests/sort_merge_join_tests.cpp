// CHUNK 12 tests: sort-merge join (SPEC §M2.3 sort_merge_join). The spec's
// golden example (R.b = [1, 2, 2, 3], S.b = [2, 2, 4] -> 4 rows), a run of
// equal keys larger than B, equality-only conditions, the same bag as the
// nested loop join, and the measured I/O written out against the formula.

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
#include "db/exec/counters.h"
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
    // `op`(seqscan left, seqscan right) with `condition`.
    std::string joinPlan(const std::string& op, const std::string& condition, const std::string& left = "R",
                         const std::string& right = "S")
    {
        return R"({"op": ")" + op + R"(", "condition": ")" + condition +
               R"(", "left": {"op": "seqscan", "table": ")" + left +
               R"("}, "right": {"op": "seqscan", "table": ")" + right + R"("}})";
    }

    struct Result
    {
        std::vector<std::string> rows; // each row as "v1|v2|...", sorted (a bag)
        std::size_t rowCount = 0;
        std::vector<TraceRow> trace;
        ExecCounters totals;
        std::size_t maxFrames = 0;
    };

    std::string rowText(const Tuple& row)
    {
        std::string text;
        for (std::size_t i = 0; i < row.size(); ++i)
            text += (i == 0 ? "" : "|") + row[i].toText();
        return text;
    }

    // Runs `json`; keeps the rows only if `keepRows` (a huge result is counted).
    Result runPlan(TestDb& db, const std::string& json, bool keepRows = true)
    {
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(json), ctx);
        Result result;
        plan.open();
        Tuple t;
        while (plan.next(t))
        {
            ++result.rowCount;
            if (keepRows)
                result.rows.push_back(rowText(t));
        }
        plan.close();
        std::sort(result.rows.begin(), result.rows.end());
        result.trace = plan.trace();
        result.totals = plan.totals();
        result.maxFrames = plan.maxFramesHeld();
        return result;
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

    // K(a INT, b INT), qualified by `table`.
    Schema kSchema(const std::string& table)
    {
        return Schema({{table, "a", Type::Int, 0}, {table, "b", Type::Int, 0}});
    }

    // One row per value of `bs`: a = its position, b = the value.
    std::vector<Tuple> kRows(const std::vector<int>& bs)
    {
        std::vector<Tuple> rows;
        for (std::size_t i = 0; i < bs.size(); ++i)
            rows.push_back({Value::ofInt(static_cast<int>(i)), Value::ofInt(bs[i])});
        return rows;
    }

    // n rows with many duplicate keys: b = (i * 7) mod `keys`.
    std::vector<Tuple> duplicateRows(int n, int keys)
    {
        std::vector<int> bs;
        for (int i = 0; i < n; ++i)
            bs.push_back((i * 7) % keys);
        return kRows(bs);
    }
} // namespace

TEST_CASE("sort_merge_join: the spec's duplicate-key example gives four rows")
{
    // R.b = [1, 2, 2, 3], S.b = [2, 2, 4]: every pairing of the two 2s with
    // the two 2s. Two rows would mean the cursor advanced instead of rewinding.
    TestDb db("smj_golden");
    db.createTable("R", kSchema("R"), kRows({1, 2, 2, 3}));
    db.createTable("S", kSchema("S"), kRows({2, 2, 4}));

    const Result result = runPlan(db, joinPlan("sort_merge_join", "R.b = S.b"));
    // Rows are (R.a, R.b, S.a, S.b): R rows 1 and 2 have b = 2, S rows 0 and 1.
    CHECK(result.rows == std::vector<std::string>{"1|2|0|2", "1|2|1|2", "2|2|0|2", "2|2|1|2"});
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("sort_merge_join: a run of equal keys larger than B completes at B = 4")
{
    // 1200 rows of key 7 on each side (5 pages each, more than B = 4) plus
    // 600 rows with other keys: the run of S is spilled to a temporary run
    // and read again for each of the 1200 R rows with key 7.
    std::vector<int> rb, sb;
    for (int i = 0; i < 1200; ++i)
    {
        rb.push_back(7);
        sb.push_back(7);
    }
    for (int i = 0; i < 600; ++i)
    {
        rb.push_back(100 + i);     // 100..699
        sb.push_back(100 + 2 * i); // even numbers 100..1298: 300 of them match
    }
    TestDb db("smj_long_run", 4);
    db.createTable("R", kSchema("R"), kRows(rb));
    db.createTable("S", kSchema("S"), kRows(sb));

    const Result result = runPlan(db, joinPlan("sort_merge_join", "R.b = S.b"), false);
    CHECK(result.rowCount == 1200 * 1200 + 300);
    CHECK(result.maxFrames <= 4);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("sort_merge_join: no matches and empty inputs give no rows")
{
    TestDb db("smj_empty");
    db.createTable("R", kSchema("R"), kRows({1, 3, 5}));
    db.createTable("S", kSchema("S"), kRows({2, 4, 6}));
    db.createTable("E", kSchema("E"), {});

    CHECK(runPlan(db, joinPlan("sort_merge_join", "R.b = S.b")).rowCount == 0);
    CHECK(runPlan(db, joinPlan("sort_merge_join", "E.b = S.b", "E", "S")).rowCount == 0);
    CHECK(runPlan(db, joinPlan("sort_merge_join", "R.b = E.b", "R", "E")).rowCount == 0);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("sort_merge_join: the same bag as nested_loop_join, with duplicates on both sides")
{
    // 400 x 300 rows over 13 key values: many duplicates on both sides.
    for (std::size_t frames : {4, 16})
    {
        CAPTURE(frames);
        TestDb db("smj_same_bag_" + std::to_string(frames), frames);
        db.createTable("R", kSchema("R"), duplicateRows(400, 13));
        db.createTable("S", kSchema("S"), duplicateRows(300, 13));

        const Result merge = runPlan(db, joinPlan("sort_merge_join", "R.b = S.b"));
        const Result loop = runPlan(db, joinPlan("nested_loop_join", "R.b = S.b"));
        CHECK(merge.rows.size() > 400); // larger than either input
        CHECK(merge.rows == loop.rows);
        CHECK(merge.maxFrames <= frames);
    }
}

TEST_CASE("sort_merge_join: a two-column key equals the nested loop result")
{
    TestDb db("smj_two_keys");
    db.createTable("R", kSchema("R"), duplicateRows(200, 5));
    db.createTable("S", kSchema("S"), duplicateRows(150, 5));
    // R.a = S.a and R.b = S.b: a is the row number, so each row matches at most one.
    const Result merge = runPlan(db, joinPlan("sort_merge_join", "R.b = S.b and R.a = S.a"));
    const Result loop = runPlan(db, joinPlan("nested_loop_join", "R.b = S.b and R.a = S.a"));
    CHECK(merge.rows.size() == 150);
    CHECK(merge.rows == loop.rows);
}

TEST_CASE("sort_merge_join: only equalities between left and right columns, same types")
{
    TestDb db("smj_errors");
    db.createTable("R", rSchema("R"), rRows(10));
    db.createTable("S", rSchema("S"), rRows(10));
    db.coldPool();

    for (const std::string condition : {"R.a < S.a", "R.a = 5", "R.a = S.a or R.c = S.c", "R.a = S.a and R.c > 3"})
    {
        CAPTURE(condition);
        const std::optional<Error> error = buildError(db, joinPlan("sort_merge_join", condition));
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Schema);
        CHECK(std::string(error->what()).find("Node 'sort_merge_join'") != std::string::npos);
    }
    const std::optional<Error> types = buildError(db, joinPlan("sort_merge_join", "R.a = S.b"));
    REQUIRE(types.has_value());
    CHECK(types->category() == ErrorCategory::Type);
    CHECK(db.engine().pool().stats().reads == 0); // rejected before any work
}

TEST_CASE("sort_merge_join: measured I/O at M = 10, N = 4 pages, B = 4")
{
    // Shares at B = 4 (phase rule): each sort's pass 0 makes runs of 3 pages
    // (its input scan holds 1 frame) and merges with 4 (fan-in 3). During
    // the join's merge, 1 page holds the group and the other 3 read the two
    // sorts' final merges: 2 runs for the left, 1 for the right.
    //   Left, 10 pages: runs 3, 3, 3, 1 (10 writes); one merge pass leaves 2
    //   runs, keys 0..2159 (9 pages) and 2160..2399 (1 page) (10 reads, 10
    //   writes). Right, 4 pages: runs 3, 1 (4 writes); one merge pass leaves
    //   1 run (4 reads, 4 writes). Writes: 28.
    //   The merge stops when S ends (§M2.3 "while r and s"), at key 959, so
    //   the left's final merge reads only 5 pages of its first run (keys up
    //   to 1199) and the first page of its second: 6 of 10. The right's
    //   final merge reads all 4. Reads: 10 + 4 + 6 + 4 = 24.
    // The scans read 10 + 1 and 4 + 1 (one directory page each): subtree
    // 40 reads, 28 writes.
    // §M4.4's formula: sort(10) + sort(4) + 10 + 4 = 30 + 4 + 14 = 48 pages.
    // Measured: 40 + 28 = 68. The gap: the two sorts stream side by side, so
    // each gets only part of B for its final merge (the left needs one more
    // pass, the right one written run), pass 0 runs are B - 1 pages, and the
    // merge may stop before reading all of one input.
    TestDb db("smj_cost", 4);
    db.createTable("R", rSchema("R"), rRows(2400)); // 10 pages
    db.createTable("S", rSchema("S"), rRows(960));  // 4 pages
    db.coldPool();

    const Result result = runPlan(db, joinPlan("sort_merge_join", "R.a = S.a"));
    CHECK(result.rowCount == 960);
    CHECK(result.trace[0].counters.pageReads == 24);
    CHECK(result.trace[0].counters.pageWrites == 28);
    CHECK(result.trace[0].subtree.pageReads == 40);
    CHECK(result.trace[0].subtree.pageWrites == 28);
    CHECK(result.maxFrames <= 4);
    CHECK(db.engine().liveTempFileCount() == 0);
}
