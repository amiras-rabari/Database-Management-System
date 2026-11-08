// CHUNK 11 tests: block nested loop join (SPEC §M2.3 nested_loop_join).
// The spec's numbers checked here: comparisons = |R| * |S| whatever B, the
// condition or the matches; page reads = M + ceil(M / (B-2)) * N; output
// schema = left then right (§M2.1); duplicate names and INT vs VARCHAR are
// errors before any page is read.
//
// R and S use rSchema / rRows (test_db.h): a record is 13 bytes, so 240 rows
// fill one data page.

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
    // nested_loop_join(seqscan left, seqscan right) with `condition`.
    std::string joinPlan(const std::string& condition, const std::string& left = "R", const std::string& right = "S")
    {
        return R"({"op": "nested_loop_join", "condition": ")" + condition +
               R"(", "left": {"op": "seqscan", "table": ")" + left +
               R"("}, "right": {"op": "seqscan", "table": ")" + right + R"("}})";
    }

    // A finished run of a plan: its rows, schema, trace and peak frames.
    struct Result
    {
        Schema schema;
        std::vector<Tuple> rows;
        std::vector<TraceRow> trace; // [0] join, [1] seqscan left, [2] seqscan right
        ExecCounters totals;
        std::size_t maxFrames = 0;
    };

    Result runPlan(TestDb& db, const std::string& json)
    {
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(json), ctx);
        Result result{plan.schema(), {}, {}, {}, 0};
        plan.open();
        Tuple t;
        while (plan.next(t))
            result.rows.push_back(t);
        plan.close();
        result.trace = plan.trace();
        result.totals = plan.totals();
        result.maxFrames = plan.maxFramesHeld();
        return result;
    }

    // The db::Error that building `json` throws, or nullopt.
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

    // n rows that all have the same c (= 1), so R.c = S.c matches every pair.
    std::vector<Tuple> sameKeyRows(int n)
    {
        std::vector<Tuple> rows;
        for (int i = 0; i < n; ++i)
            rows.push_back({Value::ofInt(i), Value::ofVarchar("k"), Value::ofInt(1)});
        return rows;
    }
} // namespace

TEST_CASE("nested_loop_join: comparisons are exactly |R| * |S| at every B, for = and <")
{
    // |R| = 100, |S| = 37: 3,700 pairs. R.a = S.a matches 37 of them (a runs
    // 0..36 in S). R.b < S.b is 'csc' < 'eng': 50 R rows with 'csc' times 18
    // S rows with 'eng' (the odd a in 0..36) = 900 matches. The count of
    // comparisons is the same in every case: a match is unknown until the
    // pair is compared.
    for (std::size_t frames : {4, 8, 16})
    {
        CAPTURE(frames);
        TestDb db("nlj_comparisons_" + std::to_string(frames), frames);
        db.createTable("R", rSchema("R"), rRows(100));
        db.createTable("S", rSchema("S"), rRows(37));

        const Result equal = runPlan(db, joinPlan("R.a = S.a"));
        CHECK(equal.rows.size() == 37);
        CHECK(equal.trace[0].counters.comparisons == 3700);
        CHECK(equal.totals.comparisons == 3700); // the scans compare nothing

        const Result less = runPlan(db, joinPlan("R.b < S.b"));
        CHECK(less.rows.size() == 900);
        CHECK(less.trace[0].counters.comparisons == 3700);
        CHECK(less.maxFrames <= frames);
    }
}

TEST_CASE("nested_loop_join: page reads follow M + ceil(M / (B-2)) * N")
{
    // R = 2400 rows = 10 data pages (M), S = 960 rows = 4 data pages (N).
    // The block is B - 2 pages = (B - 2) * 240 tuples:
    //   B = 4: block 2 pages, ceil(10/2) = 5 blocks, formula 10 + 5*4 = 30.
    //   B = 6: block 4 pages, ceil(10/4) = 3 blocks, formula 10 + 3*4 = 22.
    // Measured adds the directory page of each scan (read once per scan,
    // storage fix of chunk 07): R is scanned once (10 + 1), S once per block
    // (4 + 1 each). The block takes every frame the scans do not hold, so no
    // S page stays cached from one block to the next.
    struct Case
    {
        std::size_t frames;
        std::size_t blocks;
        std::size_t formula;
    };
    for (const Case c : {Case{4, 5, 30}, Case{6, 3, 22}})
    {
        CAPTURE(c.frames);
        TestDb db("nlj_reads_" + std::to_string(c.frames), c.frames);
        db.createTable("R", rSchema("R"), rRows(2400));
        db.createTable("S", rSchema("S"), rRows(960));
        db.coldPool();

        const Result result = runPlan(db, joinPlan("R.a = S.a"));
        CHECK(result.rows.size() == 960);
        CHECK(result.trace[1].subtree.pageReads == 10 + 1);
        CHECK(result.trace[2].subtree.pageReads == c.blocks * (4 + 1));
        CHECK(result.totals.pageReads == c.formula + 1 + c.blocks);
        CHECK(result.totals.pageWrites == 0);
        // The inner is reopened once per block; its tuples_out adds up over
        // every reopen (§M2.1: tuples it returned from next).
        CHECK(result.trace[2].counters.tuplesOut == c.blocks * 960);
        CHECK(result.trace[0].counters.comparisons == 2400 * 960);
        CHECK(result.maxFrames == c.frames);
        CHECK(db.engine().liveTempFileCount() == 0);
    }
}

TEST_CASE("nested_loop_join: an inequality gives exactly the expected pairs, left columns first")
{
    // a runs 0..3 on both sides. R.a < S.a: (0,1) (0,2) (0,3) (1,2) (1,3) (2,3).
    TestDb db("nlj_inequality");
    db.createTable("R", rSchema("R"), rRows(4));
    db.createTable("S", rSchema("S"), rRows(4));

    const Result result = runPlan(db, joinPlan("R.a < S.a"));

    std::vector<std::string> names;
    for (const Column& column : result.schema.columns())
        names.push_back(column.qualifiedName());
    CHECK(names == std::vector<std::string>{"R.a", "R.b", "R.c", "S.a", "S.b", "S.c"});

    std::vector<std::string> pairs;
    for (const Tuple& row : result.rows)
    {
        REQUIRE(row.size() == 6);
        pairs.push_back(row[0].toText() + "," + row[3].toText());
    }
    std::sort(pairs.begin(), pairs.end());
    CHECK(pairs == std::vector<std::string>{"0,1", "0,2", "0,3", "1,2", "1,3", "2,3"});
    // Each output row is the whole R row, then the whole S row.
    CHECK(result.rows[0] == Tuple{Value::ofInt(0), Value::ofVarchar("csc"), Value::ofInt(0), Value::ofInt(1),
                                  Value::ofVarchar("eng"), Value::ofInt(1)});
    CHECK(result.trace[0].counters.comparisons == 16);
}

TEST_CASE("nested_loop_join: duplicate names and INT vs VARCHAR are rejected while building")
{
    TestDb db("nlj_errors");
    db.createTable("R", rSchema("R"), rRows(10));
    db.createTable("S", rSchema("S"), rRows(10));
    db.coldPool();

    // R joined with R without a rename: R.a twice in the output schema.
    const std::optional<Error> duplicate = buildError(db, joinPlan("R.a = R.c", "R", "R"));
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->category() == ErrorCategory::Schema);

    const std::optional<Error> types = buildError(db, joinPlan("R.a = S.b"));
    REQUIRE(types.has_value());
    CHECK(types->category() == ErrorCategory::Type);

    const std::optional<Error> unknown = buildError(db, joinPlan("R.a = S.zz"));
    REQUIRE(unknown.has_value());
    CHECK(unknown->category() == ErrorCategory::Name);

    // Rejected before any execution work (§M2.2): nothing read.
    CHECK(db.engine().pool().stats().reads == 0);
}

TEST_CASE("nested_loop_join: empty inputs return cleanly")
{
    TestDb db("nlj_empty");
    db.createTable("R", rSchema("R"), rRows(100));
    db.createTable("S", rSchema("S"), rRows(37));
    db.createTable("E", rSchema("E"), {});
    db.coldPool();

    // Empty outer: zero blocks, so the inner is never opened. Reads are the
    // outer's only (its directory page): ceil(0 / (B-2)) * N = 0.
    const Result emptyOuter = runPlan(db, joinPlan("E.a = S.a", "E", "S"));
    CHECK(emptyOuter.rows.empty());
    CHECK(emptyOuter.trace[2].counters.tuplesOut == 0);
    CHECK(emptyOuter.trace[2].subtree.pageReads == 0);
    CHECK(emptyOuter.totals.pageReads == emptyOuter.trace[1].subtree.pageReads);
    CHECK(emptyOuter.trace[0].counters.comparisons == 0);

    // Empty inner: one block, one scan of an empty table, no pair.
    const Result emptyInner = runPlan(db, joinPlan("R.a = E.a", "R", "E"));
    CHECK(emptyInner.rows.empty());
    CHECK(emptyInner.trace[0].counters.comparisons == 0);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("nested_loop_join: an output larger than both inputs completes at B = 4")
{
    // 50 x 50 rows, every pair matches: 2,500 rows from inputs of 50.
    TestDb db("nlj_large_output", 4);
    db.createTable("R", rSchema("R"), sameKeyRows(50));
    db.createTable("S", rSchema("S"), sameKeyRows(50));

    const Result result = runPlan(db, joinPlan("R.c = S.c"));
    CHECK(result.rows.size() == 2500);
    CHECK(result.trace[0].counters.tuplesOut == 2500);
    CHECK(result.trace[0].counters.comparisons == 2500);
    CHECK(result.maxFrames <= 4);
    CHECK(db.engine().liveTempFileCount() == 0);
    // TestDb's destructor checks that no page is left pinned.
}
