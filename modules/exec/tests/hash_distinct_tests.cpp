// CHUNK 09 tests: our deterministic hash, the hash partitioner, the
// in-memory table, and distinct in hash mode (SPEC §M2.3). Choices tested
// here (exec STATUS.md, chunk 09):
// - a partition with too many different tuples is sorted (Q1 (a));
// - only the table's entries are counted against B (Q2 (a));
// - on overflow the table is spilled to one run first (Q3 (a)).
// Tables are one INT column x: a record is 4 bytes, so a page holds
// floor(4088 / (4 + 4)) = 511 rows, and 15330 rows are 30 pages.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/exec_context.h"
#include "db/exec/executable_plan.h"
#include "db/exec/frame_plan.h"
#include "db/storage/temp_file.h"
#include "join/hash_partitioner.h"
#include "join/hashing.h"
#include "operators/hash_distinct.h"
#include "operators/pipelined.h"
#include "sort/run_file.h"
#include "tests/test_db.h"

using namespace db;
using namespace db::exec;
using db::exec::test::drain;
using db::exec::test::TestDb;

namespace
{
    Schema intSchema(const std::string& table)
    {
        return Schema({{table, "x", Type::Int, 0}});
    }

    void createIntTable(TestDb& db, const std::string& name, const std::vector<int>& values)
    {
        std::vector<Tuple> rows;
        for (int v : values)
            rows.push_back({Value::ofInt(v)});
        db.createTable(name, intSchema(name), rows);
    }

    std::vector<int> ints(const std::vector<Tuple>& rows)
    {
        std::vector<int> values;
        for (const Tuple& row : rows)
            values.push_back(row[0].asInt());
        return values;
    }

    std::string distinctOf(const std::string& table, const std::string& mode)
    {
        return R"({"op": "distinct", "mode": ")" + mode + R"(", "child": {"op": "seqscan", "table": ")" + table
               + R"("}})";
    }

    // Runs the plan to the end; also returns its trace through `trace`.
    std::vector<Tuple> runPlan(TestDb& db, const std::string& json, std::vector<TraceRow>* trace = nullptr,
                               std::size_t* maxFrames = nullptr)
    {
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(json), ctx);
        std::vector<Tuple> rows;
        plan.open();
        Tuple t;
        while (plan.next(t))
            rows.push_back(t);
        plan.close();
        if (trace != nullptr)
            *trace = plan.trace();
        if (maxFrames != nullptr)
            *maxFrames = plan.maxFramesHeld();
        return rows;
    }

    // 15330 rows (30 pages) with values 0..2999, each about 5 times, mixed.
    std::vector<int> manyDuplicates()
    {
        std::vector<int> values;
        for (int i = 0; i < 15330; ++i)
            values.push_back((i * 7) % 3000);
        return values;
    }

    std::vector<int> sortedUnique(std::vector<int> values)
    {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }
} // namespace

TEST_CASE("hashing: FNV-1a steps match the published test vectors")
{
    // FNV-1a 64 of "a" and of "foobar" (the reference test vectors).
    CHECK(fnv1aAddByte(kFnvOffsetBasis, 'a') == 0xAF63DC4C8601EC8CULL);
    std::uint64_t hash = kFnvOffsetBasis;
    for (char c : std::string("foobar"))
        hash = fnv1aAddByte(hash, static_cast<std::uint8_t>(c));
    CHECK(hash == 0x85944171F73967E8ULL);
    // Zero bytes leave the start value unchanged.
    CHECK(kFnvOffsetBasis == 14695981039346656037ULL);
}

TEST_CASE("hashing: a fixed key gives a fixed value; seeds give different functions")
{
    const Tuple t = {Value::ofInt(42), Value::ofVarchar("bob")};
    const std::vector<std::size_t> both = {0, 1};
    // The constant was computed once with this code; the arithmetic is plain
    // 64-bit unsigned math and the bytes are built with shifts, so every
    // platform gives the same number (check on Linux at the milestone run).
    CHECK(hashKey(t, both, kSeedPartition) == 7803739889033658931ULL);
    CHECK(hashKey(t, both, kSeedPartition) != hashKey(t, both, kSeedTable));
    // Equal keys, equal hashes; only the key columns count.
    const Tuple same = {Value::ofInt(42), Value::ofVarchar("bob")};
    const Tuple otherName = {Value::ofInt(42), Value::ofVarchar("ann")};
    CHECK(hashKey(t, both, kSeedTable) == hashKey(same, both, kSeedTable));
    CHECK(hashKey(t, {0}, kSeedTable) == hashKey(otherName, {0}, kSeedTable));
    CHECK(hashKey(t, both, kSeedTable) != hashKey(otherName, both, kSeedTable));
}

TEST_CASE("hashing: h2 spreads the keys of one h1 partition over all buckets")
{
    // Keys 0..9999; the keys of partition 0 out of 4 by h1 go into 512
    // buckets by h2. If h2 were h1, they would all land in buckets 0, 4, 8,
    // ... (a quarter of them), about 4 times too many per bucket.
    std::vector<std::size_t> bucketSizes(512, 0);
    std::size_t inPartition = 0;
    for (int key = 0; key < 10000; ++key)
    {
        const Tuple t = {Value::ofInt(key)};
        if (hashKey(t, {0}, kSeedPartition) % 4 != 0)
            continue;
        ++inPartition;
        ++bucketSizes[hashKey(t, {0}, kSeedTable) % 512];
    }
    CHECK(inPartition > 2300);
    CHECK(inPartition < 2700);
    CHECK(*std::max_element(bucketSizes.begin(), bucketSizes.end()) <= 15); // average about 5
}

TEST_CASE("partitioner: 10000 keys into P = 3, each key in exactly one partition, the same every time")
{
    TestDb db("hash_partitioner");
    const Schema schema = intSchema("A");
    std::vector<std::size_t> firstSizes;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const std::uint64_t writesBefore = db.engine().pool().stats().writes;
        std::vector<storage::TempFile> runs;
        {
            HashPartitioner partitioner(db.engine(), schema, {0}, 3, kSeedPartition);
            for (int key = 0; key < 10000; ++key)
                partitioner.add({Value::ofInt(key)});
            runs = partitioner.finish();
        }
        REQUIRE(runs.size() == 3);
        std::size_t pages = 0;
        std::vector<std::size_t> sizes;
        std::vector<int> seen;
        for (const storage::TempFile& run : runs)
        {
            pages += run.numPages();
            RunReader reader(db.engine(), run, schema);
            Tuple t;
            std::size_t count = 0;
            while (reader.next(t))
            {
                seen.push_back(t[0].asInt());
                ++count;
            }
            sizes.push_back(count);
            CHECK(count > 3000); // well spread: about 3333 each
            CHECK(count < 3700);
        }
        std::sort(seen.begin(), seen.end());
        std::vector<int> all(10000);
        for (int key = 0; key < 10000; ++key)
            all[key] = key;
        CHECK(seen == all); // every key once
        CHECK(db.engine().pool().stats().writes - writesBefore == pages);
        if (attempt == 0)
            firstSizes = sizes;
        else
            CHECK(sizes == firstSizes);
    }
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("hash distinct: everything fits in memory, first-occurrence order, no I/O of its own")
{
    TestDb db("hash_distinct_memory");
    createIntTable(db, "A", {3, 1, 3, 2, 1});
    db.coldPool();
    std::vector<TraceRow> trace;
    CHECK(ints(runPlan(db, distinctOf("A", "hash"), &trace)) == std::vector<int>{3, 1, 2});
    CHECK(trace[0].label == "distinct");
    CHECK(trace[0].counters.tuplesOut == 3);
    CHECK(trace[0].counters.pageReads == 0); // own I/O; the scan's 2 reads are its child's
    CHECK(trace[0].counters.pageWrites == 0);
    CHECK(trace[0].counters.comparisons > 0); // the duplicates 3 and 1 were compared
}

TEST_CASE("hash distinct: 30 pages with many duplicates at B = 4 overflow and partition; same result and trace every run")
{
    TestDb db("hash_distinct_overflow");
    const std::vector<int> values = manyDuplicates();
    createIntTable(db, "A", values);
    const std::vector<int> expected = sortedUnique(values);

    std::vector<TraceRow> firstTrace;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        db.coldPool();
        std::vector<TraceRow> trace;
        std::size_t maxFrames = 0;
        // The table (3 pages, 1533 entries) fills before the 3000 values are
        // seen, so the operator partitions; each partition (~1000 values) then
        // fits a table of 3 pages.
        std::vector<int> got = ints(runPlan(db, distinctOf("A", "hash"), &trace, &maxFrames));
        std::sort(got.begin(), got.end());
        CHECK(got == expected); // no duplicate, nothing lost
        CHECK(trace[0].counters.pageWrites > 0);
        CHECK(maxFrames <= 4);
        CHECK(db.engine().liveTempFileCount() == 0);
        if (attempt == 0)
            firstTrace = trace;
        else
            CHECK(trace == firstTrace); // page counts and comparisons included
    }

    // The same set as sort mode gives.
    db.coldPool();
    CHECK(ints(runPlan(db, distinctOf("A", "sort"))) == expected);
}

TEST_CASE("hash distinct: 30 pages of one value keep one table entry and write nothing")
{
    TestDb db("hash_distinct_skew");
    createIntTable(db, "A", std::vector<int>(15330, 7));
    db.coldPool(); // else reserving a frame may write a page left dirty by the load
    std::vector<TraceRow> trace;
    CHECK(ints(runPlan(db, distinctOf("A", "hash"), &trace)) == std::vector<int>{7});
    CHECK(trace[0].counters.pageWrites == 0);
}

TEST_CASE("hash distinct: partitions with too many different values are sorted instead (smallest B)")
{
    // B = 3: the table gets ownFrames = 2 pages (1022 entries), P = 2
    // partitions of about 3800 different values each (every value twice),
    // and per partition a table of 3 - 1 = 2 pages: both partitions
    // overflow and are sorted, and the sort's duplicates must be skipped.
    TestDb db("hash_distinct_fallback", 3);
    std::vector<int> values;
    for (int i = 0; i < 15330; ++i)
        values.push_back((i * 7919) % 15330 / 2); // 0..7664, each twice, mixed (7919 and 15330 share no factor)
    createIntTable(db, "A", values);
    db.coldPool();

    const PlanNode plan = physicalFromJsonText(distinctOf("A", "hash"));
    const FramePlan frames = divideFrames(plan, 3);
    CHECK(frames.shares[0] == FrameShare{3, 3, 2});

    ExecContext ctx(db.engine());
    HashDistinctOperator op(ctx, std::make_unique<SeqScanOperator>(ctx, "A"), frames.shares[0]);
    db.engine().pool().resetPeak();
    std::vector<int> got = ints(drain(op));
    CHECK(op.sortedPartitions() == 2);
    std::sort(got.begin(), got.end());
    CHECK(got == sortedUnique(values));
    CHECK(db.engine().pool().peakPinnedCount() <= 3);
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("hash distinct: needs at least 3 frames (a partition may have to be sorted)")
{
    const PlanNode plan = physicalFromJsonText(distinctOf("A", "hash"));
    CHECK(divideFrames(plan, 3).minimumFrames == 3);
    CHECK_THROWS_AS(divideFrames(plan, 2), Error);
}

TEST_CASE("hash distinct: an empty input ends at once with no I/O of its own")
{
    TestDb db("hash_distinct_empty");
    createIntTable(db, "A", {});
    db.coldPool();
    std::vector<TraceRow> trace;
    CHECK(runPlan(db, distinctOf("A", "hash"), &trace).empty());
    CHECK(trace[0].counters.pageWrites == 0);
    CHECK(trace[0].counters.pageReads == 0);
}
