// CHUNK 13 tests: Grace hash join (SPEC §M2.3 hash_join): the same bag as
// the other joins, P = B - 1, the partition/probe page accounting, overflow
// handled by block nested loop on that partition pair (chunk 13 Q1 (b)),
// the skew of §M2.6 measurement 4, empty inputs, equality only.

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
    std::string scan(const std::string& table)
    {
        return R"({"op": "seqscan", "table": ")" + table + R"("})";
    }

    std::string join(const std::string& op, const std::string& condition, const std::string& left,
                     const std::string& right)
    {
        return R"({"op": ")" + op + R"(", "condition": ")" + condition + R"(", "left": )" + left +
               R"(, "right": )" + right + "}";
    }

    struct Result
    {
        std::vector<std::string> rows; // sorted (a bag); empty if not kept
        std::size_t rowCount = 0;
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
        result.maxFrames = plan.maxFramesHeld();
        return result;
    }

    // K(a INT, b INT), qualified by `table`; a = row number, b = the value.
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

    std::size_t sum(const PlanJson& numbers)
    {
        std::size_t total = 0;
        for (const PlanJson& n : numbers)
            total += n.get<std::size_t>();
        return total;
    }
} // namespace

TEST_CASE("hash_join: the spec's plan gives the same bag as the other two joins")
{
    // hash_join(filter(seqscan R) on R.a > 500, seqscan S) on an INT key
    // with duplicates on both sides (c = i mod 7).
    TestDb db("hj_same_bag", 4);
    db.createTable("R", rSchema("R"), rRows(1000));
    db.createTable("S", rSchema("S"), rRows(300));
    const std::string left = R"({"op": "filter", "condition": "R.a > 500", "child": )" + scan("R") + "}";

    const Result hash = runPlan(db, join("hash_join", "R.c = S.c", left, scan("S")));
    const Result loop = runPlan(db, join("nested_loop_join", "R.c = S.c", left, scan("S")));
    const Result merge = runPlan(db, join("sort_merge_join", "R.c = S.c", left, scan("S")));
    CHECK(hash.rows.size() > 1000); // larger than either input
    CHECK(hash.rows == loop.rows);
    CHECK(hash.rows == merge.rows);
    CHECK(hash.maxFrames <= 4);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("hash_join: no overflow at B = 16: writes = probe reads = the partitions' pages")
{
    // M = N = 24 pages, P = 15. Partitioning writes every partition page
    // once, probing reads each once: the join's own reads and writes are
    // both the partitions' total. That total is a little above M + N = 48,
    // because each partition's last page is only partly full; the scans
    // read 24 + 1 each (one directory page). §M2.5's example
    // ("hash_join reads=96 writes=48") assumes full pages.
    TestDb db("hj_cost", 16);
    db.createTable("R", rSchema("R"), rRows(5760)); // 24 pages
    db.createTable("S", rSchema("S"), rRows(5760));
    db.coldPool();

    const Result result = runPlan(db, join("hash_join", "R.a = S.a", scan("R"), scan("S")), false);
    CHECK(result.rowCount == 5760);
    const PlanJson& details = result.trace[0].details;
    CHECK(details["partitions"] == 15);
    CHECK(details["overflow_partitions"].empty());
    const std::size_t partitionPages = sum(details["left_pages"]) + sum(details["right_pages"]);
    CHECK(partitionPages >= 48);
    CHECK(partitionPages <= 48 + 2 * 15); // at most one partly full page per partition
    CHECK(result.trace[0].counters.pageWrites == partitionPages);
    CHECK(result.trace[0].counters.pageReads == partitionPages);
    CHECK(result.trace[0].subtree.pageReads == partitionPages + 25 + 25);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("hash_join: at B = 4, P = 3, and partitions too big for the table use block nested loop")
{
    // R = 30 pages: partitions of about 10 pages, more than the table's 3.
    TestDb db("hj_uniform_overflow", 4);
    db.createTable("R", rSchema("R"), rRows(7200));
    db.createTable("S", rSchema("S"), rRows(2400));

    const Result result = runPlan(db, join("hash_join", "R.a = S.a", scan("R"), scan("S")), false);
    CHECK(result.rowCount == 2400);
    const PlanJson& details = result.trace[0].details;
    CHECK(details["partitions"] == 3);
    CHECK(details["overflow_partitions"] == PlanJson::array({0, 1, 2}));
    CHECK(result.maxFrames <= 4);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("hash_join: one key in 40 percent of the rows on both sides (skew)")
{
    // R: 5000 rows, 2000 with b = 7 (about 6 pages); S: 2500 rows, 1000 with
    // b = 7. Every b = 7 row lands in one partition, which overflows at B = 4.
    // The other R keys are 1000..3999, the other S keys 1000, 1002, ...,
    // 3998: 1500 of them match.
    std::vector<int> rb, sb;
    for (int i = 0; i < 5000; ++i)
        rb.push_back(i < 2000 ? 7 : 1000 + (i - 2000));
    for (int i = 0; i < 2500; ++i)
        sb.push_back(i < 1000 ? 7 : 1000 + 2 * (i - 1000));
    TestDb db("hj_skew", 4);
    db.createTable("R", kSchema("R"), kRows(rb));
    db.createTable("S", kSchema("S"), kRows(sb));

    const std::string plan = join("hash_join", "R.b = S.b", scan("R"), scan("S"));
    db.coldPool();
    const Result first = runPlan(db, plan, false);
    CHECK(first.rowCount == 2000 * 1000 + 1500);
    const PlanJson& details = first.trace[0].details;
    REQUIRE_FALSE(details["overflow_partitions"].empty());
    const std::size_t skewed = details["overflow_partitions"][0].get<std::size_t>();
    CHECK(details["left_pages"][skewed].get<std::size_t>() > 3);
    CHECK(first.maxFrames <= 4);

    // Deterministic: the same partitions and counters on a second run from
    // the same start (a cold pool).
    db.coldPool();
    const Result second = runPlan(db, plan, false);
    CHECK(second.trace == first.trace);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("hash_join: empty build side and empty probe side give no rows")
{
    TestDb db("hj_empty");
    db.createTable("R", rSchema("R"), rRows(100));
    db.createTable("E", rSchema("E"), {});
    CHECK(runPlan(db, join("hash_join", "E.a = R.a", scan("E"), scan("R"))).rowCount == 0);
    CHECK(runPlan(db, join("hash_join", "R.a = E.a", scan("R"), scan("E"))).rowCount == 0);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("hash_join: only equalities between left and right columns")
{
    TestDb db("hj_errors");
    db.createTable("R", rSchema("R"), rRows(10));
    db.createTable("S", rSchema("S"), rRows(10));
    ExecContext ctx(db.engine());
    try
    {
        buildExecutablePlan(physicalFromJsonText(join("hash_join", "R.a < S.a", scan("R"), scan("S"))), ctx);
        FAIL("an inequality was accepted");
    }
    catch (const Error& error)
    {
        CHECK(error.category() == ErrorCategory::Schema);
        CHECK(std::string(error.what()).find(
                  "Node 'hash_join': condition must be equalities between left and right columns.") !=
              std::string::npos);
    }
}

TEST_CASE("every plan runs at B = 4: sort over union_all with a blocking right input")
{
    // The right input (a sort over a hash join) needs 4 frames when opened;
    // union_all writes it to a run first (FrameShare::writeInner).
    TestDb db("union_all_fit", 4);
    db.createTable("R", rSchema("R"), rRows(600));
    db.createTable("S", rSchema("S"), rRows(300));
    const std::string hj = join("hash_join", "R.c = S.c", scan("R"), scan("S"));
    const std::string json = R"({"op": "sort", "keys": [["R.a", "asc"]], "child": {"op": "union_all", "left": )" +
                             hj + R"(, "right": {"op": "sort", "keys": [["S.a", "desc"]], "child": )" + hj + "}}}";
    const Result alone = runPlan(db, hj, false);
    const Result result = runPlan(db, json, false);
    CHECK(result.rowCount == 2 * alone.rowCount);
    CHECK(result.maxFrames <= 4);
    CHECK(db.engine().liveTempFileCount() == 0);
}
