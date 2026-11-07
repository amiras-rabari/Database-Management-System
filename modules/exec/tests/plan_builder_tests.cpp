// CHUNK 05 tests: building a physical plan into operators (every check
// before any page is read), running it through ExecutablePlan, and the
// trace in pre-order. Page counts assume LRU with B = 4 and a cold pool.

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/counters.h"
#include "db/exec/exec_context.h"
#include "db/exec/executable_plan.h"
#include "db/storage/io_stats.h"
#include "tests/test_db.h"

using namespace db;
using namespace db::exec;
using db::exec::test::rRows;
using db::exec::test::rSchema;
using db::exec::test::TestDb;

namespace
{
    // R = 2,400 rows (10 data pages), S = 5 rows with R's columns renamed
    // to S, then a cold pool.
    struct RAndS
    {
        TestDb db;
        explicit RAndS(const std::string& name, std::size_t frames = 4) : db(name, frames)
        {
            db.createTable("R", rSchema("R"), rRows(2400));
            db.createTable("S", rSchema("S"), rRows(5));
            db.coldPool();
        }
    };

    ExecutablePlan build(const std::string& json, ExecContext& ctx)
    {
        return buildExecutablePlan(physicalFromJsonText(json), ctx);
    }

    // open, next until the end, close; returns every result tuple.
    std::vector<Tuple> run(ExecutablePlan& plan)
    {
        std::vector<Tuple> rows;
        plan.open();
        Tuple t;
        while (plan.next(t))
            rows.push_back(t);
        plan.close();
        return rows;
    }

    // Expects building `json` to fail with `category`, before any page is
    // read and without making a temporary file.
    void checkRejectedBeforeWork(RAndS& fx, const std::string& json, ErrorCategory category)
    {
        ExecContext ctx(fx.db.engine());
        const storage::IoStats before = fx.db.engine().pool().stats();
        try
        {
            build(json, ctx);
            FAIL("expected db::Error");
        }
        catch (const Error& e)
        {
            CHECK(e.category() == category);
        }
        CHECK(fx.db.engine().pool().stats() == before);
        CHECK(fx.db.engine().liveTempFileCount() == 0);
    }

    const char* const kProjectFilterScan = R"({"op": "project", "columns": ["R.a"],
        "child": {"op": "filter", "condition": "R.a > 500",
                  "child": {"op": "seqscan", "table": "R"}}})";

    const char* const kUnionAll = R"({"op": "union_all",
        "left": {"op": "filter", "condition": "R.a > 500",
                 "child": {"op": "seqscan", "table": "R"}},
        "right": {"op": "seqscan", "table": "S"}})";
} // namespace

TEST_CASE("plan builder: a plan from JSON text gives the expected rows")
{
    RAndS fx("build_json");
    ExecContext ctx(fx.db.engine());
    ExecutablePlan plan = build(kProjectFilterScan, ctx);
    CHECK(plan.schema().size() == 1);
    CHECK(plan.schema().column(0).qualifiedName() == "R.a");

    const std::vector<Tuple> rows = run(plan);
    REQUIRE(rows.size() == 1899); // a = 501 .. 2399
    CHECK(rows.front() == Tuple{Value::ofInt(501)});
    CHECK(rows.back() == Tuple{Value::ofInt(2399)});
}

TEST_CASE("plan builder: an unknown table deep in the plan is rejected before any work")
{
    RAndS fx("unknown_table");
    checkRejectedBeforeWork(fx, R"({"op": "filter", "condition": "X.a > 1",
        "child": {"op": "filter", "condition": "X.a > 2",
                  "child": {"op": "seqscan", "table": "X"}}})", ErrorCategory::Name);
}

TEST_CASE("plan builder: an unknown column is rejected before any work")
{
    RAndS fx("unknown_column");
    checkRejectedBeforeWork(fx, R"({"op": "filter", "condition": "R.zz > 1",
        "child": {"op": "seqscan", "table": "R"}})", ErrorCategory::Name);
}

TEST_CASE("plan builder: a plan that needs more than B frames is rejected before any work")
{
    // A sort-merge join needs at least 3 frames (chunk 03). The frame check
    // runs first, so the error comes before the join is built.
    RAndS fx("too_small", 2);
    checkRejectedBeforeWork(fx, R"({"op": "sort_merge_join", "condition": "R.a = S.a",
        "left": {"op": "seqscan", "table": "R"},
        "right": {"op": "seqscan", "table": "S"}})", ErrorCategory::Schema);
}

TEST_CASE("plan builder: the trace lists every node in pre-order with its depth and counters")
{
    RAndS fx("trace_order");
    ExecContext ctx(fx.db.engine());
    ExecutablePlan plan = build(kUnionAll, ctx);
    run(plan);

    const std::vector<TraceRow> rows = plan.trace();
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].label == "union_all");
    CHECK(rows[0].depth == 0);
    CHECK(rows[1].label == "filter");
    CHECK(rows[1].depth == 1);
    CHECK(rows[2].label == "seqscan R");
    CHECK(rows[2].depth == 2);
    CHECK(rows[3].label == "seqscan S");
    CHECK(rows[3].depth == 1);

    // Each row carries its own node's counters.
    CHECK(rows[0].counters.tuplesOut == 1904); // 1899 from R + 5 from S
    CHECK(rows[1].counters.comparisons == 2400);
    CHECK(rows[2].counters.tuplesOut == 2400);
    CHECK(rows[2].counters.pageReads == 11); // 10 data pages + directory page
    CHECK(rows[3].counters.pageReads == 2); // 1 data page + directory page
    CHECK(rows[1].subtree.pageReads == 11);
}

TEST_CASE("plan builder: totals add up every node; tuples_out is the number of result rows")
{
    RAndS fx("totals");
    ExecContext ctx(fx.db.engine());
    ExecutablePlan plan = build(kUnionAll, ctx);
    const std::size_t pulled = run(plan).size();

    ExecCounters sum;
    for (const TraceRow& row : plan.trace())
        sum = sum + row.counters;

    const ExecCounters totals = plan.totals();
    CHECK(totals.tuplesOut == pulled);
    CHECK(totals.tuplesOut == 1904);
    CHECK(totals.comparisons == sum.comparisons);
    CHECK(totals.pageReads == sum.pageReads);
    CHECK(totals.pageWrites == sum.pageWrites);
    CHECK(totals.pageReads == 13);

    // The root's subtree line shows the same page and comparison totals.
    const TraceRow root = plan.trace()[0];
    CHECK(root.subtree.pageReads == totals.pageReads);
    CHECK(root.subtree.pageWrites == totals.pageWrites);
    CHECK(root.subtree.comparisons == totals.comparisons);
}

TEST_CASE("plan builder: close leaves no pins and no temp files; a second close is harmless")
{
    RAndS fx("close");
    // Hold 3 frames for a moment before the plan runs: maxFramesHeld must
    // count only what the plan held (open() starts a new peak).
    fx.db.engine().pool().reserveFrames(3);
    fx.db.engine().pool().releaseFrames(3);

    ExecContext ctx(fx.db.engine());
    ExecutablePlan plan = build(kProjectFilterScan, ctx);
    run(plan);
    CHECK(fx.db.engine().pool().pinnedFrameCount() == 0);
    CHECK(fx.db.engine().liveTempFileCount() == 0);
    CHECK_NOTHROW(plan.close());
    CHECK(plan.maxFramesHeld() == 1); // the seqscan's one page (chunk 03)
}

TEST_CASE("plan builder: a plan stopped after one row, or destroyed while open, leaves no pins")
{
    RAndS fx("stopped");
    ExecContext ctx(fx.db.engine());
    Tuple t;
    {
        ExecutablePlan plan = build(R"({"op": "seqscan", "table": "R"})", ctx);
        plan.open();
        REQUIRE(plan.next(t));
        CHECK(fx.db.engine().pool().pinnedFrameCount() == 1);
        plan.close();
        CHECK(fx.db.engine().pool().pinnedFrameCount() == 0);
    }
    {
        ExecutablePlan plan = build(R"({"op": "seqscan", "table": "R"})", ctx);
        plan.open();
        REQUIRE(plan.next(t));
    } // destroyed without close()
    CHECK(fx.db.engine().pool().pinnedFrameCount() == 0);
}

TEST_CASE("plan builder: the same plan on a fresh pool gives the same trace")
{
    RAndS fx("deterministic");
    std::vector<TraceRow> first;
    {
        ExecContext ctx(fx.db.engine());
        ExecutablePlan plan = build(kUnionAll, ctx);
        run(plan);
        first = plan.trace();
    }
    fx.db.coldPool();
    ExecContext ctx(fx.db.engine());
    ExecutablePlan plan = build(kUnionAll, ctx);
    run(plan);
    CHECK(plan.trace() == first);
}

TEST_CASE("plan builder: a plan 1000 nodes deep builds, runs and traces")
{
    // kMaxNestingDepth = 1000 (common): 999 limits over one seqscan.
    TestDb db("deep");
    db.createTable("R", rSchema("R"), rRows(5));
    PlanNode node;
    node.op = PlanOp::SeqScan;
    node.table = "R";
    for (int i = 0; i < 999; ++i)
    {
        PlanNode limit;
        limit.op = PlanOp::Limit;
        limit.limitCount = 100;
        limit.children.push_back(std::move(node));
        node = std::move(limit);
    }

    ExecContext ctx(db.engine());
    ExecutablePlan plan = buildExecutablePlan(node, ctx);
    CHECK(run(plan) == rRows(5));
    const std::vector<TraceRow> rows = plan.trace();
    REQUIRE(rows.size() == 1000);
    CHECK(rows.back().label == "seqscan R");
    CHECK(rows.back().depth == 999);
}
