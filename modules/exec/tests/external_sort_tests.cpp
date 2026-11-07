// CHUNK 07 tests: the external merge sort (ExternalSorter) and the sort
// operator (SPEC §M2.3). Choices tested here (exec STATUS.md, chunk 07):
// - rewrite variant (D6): a leftover run in a merge pass is copied;
// - the output page is one of the run's frames (Q3 (a)), so runs are as
//   long as the frames given to pass 0 (3, 3 and 2 pages for N = 8, B = 3);
// - one comparison = one call of the tuple comparator (Q2 (a)).
// Rows of R are 13-byte records, 240 per page (test_db.h). Page counts
// assume LRU and a cold pool. A seqscan also reads directory page 0 once,
// so a scan of an 8-page table reads 9 pages.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/exec_context.h"
#include "db/exec/executable_plan.h"
#include "db/exec/external_sort.h"
#include "db/storage/heap_file.h"
#include "db/storage/heap_scan.h"
#include "db/storage/io_stats.h"
#include "db/storage/rid.h"
#include "tests/test_db.h"

using namespace db;
using namespace db::exec;
using db::exec::test::rRows;
using db::exec::test::rSchema;
using db::exec::test::TestDb;

namespace
{
    // The key "R.a", descending. R's rows are stored in ascending order of a,
    // so sorting them descending really has to move every row.
    std::vector<SortKey> aDescending()
    {
        return {SortKey{ColumnRef{"R", "a"}, false}};
    }

    // R's first `count` rows in descending order of a: the expected output.
    std::vector<Tuple> rRowsDescending(int count)
    {
        std::vector<Tuple> rows = rRows(count);
        std::reverse(rows.begin(), rows.end());
        return rows;
    }

    // Gives every row of table `table` to `sorter` through a heap scan, then
    // closes the scan, so its page is unpinned before the merge passes.
    void addTable(TestDb& db, const std::string& table, ExternalSorter& sorter)
    {
        storage::HeapFile heap = db.engine().openHeapFile(table);
        storage::HeapScan scan = heap.scan();
        storage::Rid rid;
        Tuple t;
        while (scan.next(rid, t))
            sorter.add(t);
        scan.close();
    }

    // Takes every tuple of the sorter's output.
    std::vector<Tuple> drainSorter(ExternalSorter& sorter)
    {
        std::vector<Tuple> rows;
        Tuple t;
        while (sorter.next(t))
            rows.push_back(t);
        return rows;
    }

    // Page reads and writes since `start`.
    storage::IoStats ioSince(TestDb& db, const storage::IoStats& start)
    {
        return db.engine().pool().stats() - start;
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

    const char* const kSortRByADesc = R"({"op": "sort", "keys": [["R.a", "desc"]],
                                          "child": {"op": "seqscan", "table": "R"}})";
} // namespace

TEST_CASE("sort: the spec's example, N = 8 pages and B = 3, rewrite variant")
{
    // 4 frames: the scan's page + the sort's 3.
    TestDb db("sort_spec_example");
    db.createTable("R", rSchema(), rRows(1920)); // 1920 / 240 = 8 data pages
    db.coldPool();
    const Schema schema = rSchema();
    const storage::IoStats start = db.engine().pool().stats();
    {
        std::uint64_t comparisons = 0;
        ExternalSorter sorter(db.engine(), schema, aDescending(), 3, 3, 3, comparisons);

        // Pass 0 while the input streams: runs of 3 and 3 pages are written;
        // the last 2 pages are still in memory. Every read so far is the scan's.
        addTable(db, "R", sorter);
        // The scan reads each page once (8 data + directory page 0), even
        // though the sort leaves it only 1 frame (storage scan, 2026-11-07).
        const std::uint64_t scanReads = ioSince(db, start).reads;
        CHECK(scanReads == 9);
        CHECK(ioSince(db, start).writes == 6);

        // finishInput writes the last run (2 pages). Pass 1 merges runs 1 and
        // 2 (6 reads, 6 writes) and rewrites run 3 (2 reads, 2 writes). Then
        // it starts the final merge, which reads the first page of each of
        // its 2 runs (+2; counted in pass 2's 8 below).
        sorter.finishInput();
        CHECK(sorter.stats().initialRuns == 3);
        CHECK(sorter.stats().passes == 3);
        CHECK(ioSince(db, start).reads - scanReads == 8 + 2);
        CHECK(ioSince(db, start).writes == 6 + 2 + 8);

        // Pass 2 merges the last 2 runs while streaming: 8 reads, 0 writes.
        CHECK(drainSorter(sorter) == rRowsDescending(1920));
        CHECK(ioSince(db, start).reads - scanReads == 16);
        CHECK(ioSince(db, start).writes == 16);
        CHECK(comparisons > 0);
        CHECK(db.engine().pool().peakPinnedCount() == 4);
    }
    // The sort's own I/O is 16 reads + 16 writes; with the 8 data pages it
    // reads that is 40 = M4's 2*M*(passes-1) + M = 2*8*2 + 8.
    CHECK(expectedSortPasses(8, 3) == 3);
    CHECK(db.engine().liveTempFileCount() == 0); // destroying the sorter deleted its runs
}

TEST_CASE("sort: sort(seqscan R) at B = 4 - runs of 3 pages, one streaming merge, same counters every run")
{
    TestDb db("sort_plan_b4");
    db.createTable("R", rSchema(), rRows(1920)); // 8 data pages

    std::vector<TraceRow> firstTrace;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        db.coldPool();
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(kSortRByADesc), ctx);

        // divideFrames gives the root sort 3 frames for pass 0 (the scan has
        // the 4th) and 4 for merging: 3 runs (3, 3, 2 pages), and 3 runs fit
        // in one merge of fan-in 3. So 2 passes: the sort writes 8 and reads 8.
        plan.open();
        CHECK(db.engine().liveTempFileCount() == 3); // the 3 runs being merged
        std::vector<Tuple> rows;
        Tuple t;
        while (plan.next(t))
            rows.push_back(t);
        plan.close();
        CHECK(rows == rRowsDescending(1920));
        CHECK(db.engine().liveTempFileCount() == 0);

        const std::vector<TraceRow> trace = plan.trace();
        REQUIRE(trace.size() == 2);
        CHECK(trace[0].label == "sort");
        CHECK(trace[0].counters.pageReads == 8);
        CHECK(trace[0].counters.pageWrites == 8);
        CHECK(trace[0].counters.tuplesOut == 1920);
        CHECK(trace[0].counters.comparisons > 0);
        CHECK(trace[1].counters.pageReads == 9); // 8 data pages + directory page 0
        CHECK(plan.totals().pageReads == 17);
        CHECK(plan.totals().pageWrites == 8);
        CHECK(plan.maxFramesHeld() == 4);

        if (attempt == 0)
            firstTrace = trace;
        else
            CHECK(trace == firstTrace); // comparisons included
    }
}

TEST_CASE("sort: an input of 20 x B pages at B = 4 sorts in 4 passes within 4 frames")
{
    TestDb db("sort_twenty_b");
    db.createTable("R", rSchema(), rRows(19200)); // 19200 / 240 = 80 data pages
    db.coldPool();
    const Schema schema = rSchema();
    const storage::IoStats start = db.engine().pool().stats();
    {
        // The frames divideFrames gives a root sort at B = 4.
        std::uint64_t comparisons = 0;
        ExternalSorter sorter(db.engine(), schema, aDescending(), 3, 4, 4, comparisons);
        addTable(db, "R", sorter);
        const std::uint64_t scanReads = ioSince(db, start).reads;
        CHECK(scanReads == 81); // 80 data pages + directory page 0, each once
        sorter.finishInput();
        CHECK(drainSorter(sorter) == rRowsDescending(19200));

        // Pass 0: ceil(80 / 3) = 27 runs. Fan-in 3: 27 -> 9 -> 3, then the
        // streaming merge of 3: 4 passes.
        CHECK(sorter.stats().initialRuns == 27);
        CHECK(sorter.stats().passes == 4);
        CHECK(expectedSortPasses(80, 4) == 4);
        // Every pass but the last writes 80 pages; every pass but the first
        // reads 80.
        CHECK(ioSince(db, start).writes == 3 * 80);
        CHECK(ioSince(db, start).reads - scanReads == 3 * 80);
        CHECK(db.engine().pool().peakPinnedCount() <= 4);
    }
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("sort: an input that fits in pass 0's frames is one run, one pass, no writes")
{
    TestDb db("sort_one_run");
    db.createTable("R", rSchema(), rRows(480)); // 2 data pages
    db.coldPool();
    const Schema schema = rSchema();
    const storage::IoStats start = db.engine().pool().stats();

    std::uint64_t comparisons = 0;
    ExternalSorter sorter(db.engine(), schema, aDescending(), 3, 3, 3, comparisons);
    addTable(db, "R", sorter);
    sorter.finishInput();
    CHECK(db.engine().liveTempFileCount() == 0); // nothing was written
    CHECK(drainSorter(sorter) == rRowsDescending(480));
    CHECK(sorter.stats().initialRuns == 1);
    CHECK(sorter.stats().passes == 1);
    CHECK(ioSince(db, start).writes == 0);
    CHECK(ioSince(db, start).reads == 3); // only the scan
}

TEST_CASE("sort: a run that does not fit in the final pass's frames is written first")
{
    // 3 pages fit in pass 0's 3 frames, but the final pass may hold only 2
    // (a sort below another operator). The run is written (3 writes) and
    // read back by the streaming pass (3 reads): 2 passes.
    TestDb db("sort_run_too_big_for_final");
    db.createTable("R", rSchema(), rRows(720)); // 3 data pages
    db.coldPool();
    const Schema schema = rSchema();
    const storage::IoStats start = db.engine().pool().stats();

    std::uint64_t comparisons = 0;
    ExternalSorter sorter(db.engine(), schema, aDescending(), 3, 3, 2, comparisons);
    addTable(db, "R", sorter);
    const std::uint64_t scanReads = ioSince(db, start).reads;
    sorter.finishInput();
    CHECK(sorter.stats().initialRuns == 1);
    CHECK(sorter.stats().passes == 2);
    CHECK(drainSorter(sorter) == rRowsDescending(720));
    CHECK(ioSince(db, start).writes == 3);
    CHECK(ioSince(db, start).reads - scanReads == 3); // the run, read back once
}

TEST_CASE("sort: an empty input gives no rows, one pass, no I/O and no temp file")
{
    TestDb db("sort_empty");
    db.createTable("R", rSchema(), {});
    db.coldPool();
    const Schema schema = rSchema();

    std::uint64_t comparisons = 0;
    ExternalSorter sorter(db.engine(), schema, aDescending(), 3, 3, 3, comparisons);
    const storage::IoStats start = db.engine().pool().stats();
    sorter.finishInput();
    Tuple t;
    CHECK_FALSE(sorter.next(t));
    CHECK(sorter.stats().initialRuns == 0);
    CHECK(sorter.stats().passes == 1);
    CHECK(comparisons == 0);
    CHECK(ioSince(db, start) == storage::IoStats{});
    CHECK(db.engine().liveTempFileCount() == 0);

    // The same through a plan: only the scan's directory page is read.
    ExecContext ctx(db.engine());
    ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(kSortRByADesc), ctx);
    CHECK(run(plan).empty());
    CHECK(plan.totals().pageWrites == 0);
    CHECK(plan.totals().pageReads == 1);
}

TEST_CASE("sort: two keys, ascending then descending; VARCHAR order is byte-wise")
{
    TestDb db("sort_keys");
    const Schema schema({{"T", "a", Type::Int, 0}, {"T", "b", Type::Varchar, 10}});
    // "B" (0x42) < "a" (0x61) < "z" (0x7A) < "é" (0xC3 0xA9 in UTF-8).
    db.createTable("T", schema,
                   {{Value::ofInt(1), Value::ofVarchar("B")},
                    {Value::ofInt(0), Value::ofVarchar("a")},
                    {Value::ofInt(1), Value::ofVarchar("a")},
                    {Value::ofInt(0), Value::ofVarchar("z")},
                    {Value::ofInt(1), Value::ofVarchar("\xC3\xA9")}});
    db.coldPool();

    ExecContext ctx(db.engine());
    ExecutablePlan plan = buildExecutablePlan(
        physicalFromJsonText(R"({"op": "sort", "keys": [["T.a", "asc"], ["T.b", "desc"]],
                                 "child": {"op": "seqscan", "table": "T"}})"),
        ctx);
    const std::vector<Tuple> expected = {{Value::ofInt(0), Value::ofVarchar("z")},
                                         {Value::ofInt(0), Value::ofVarchar("a")},
                                         {Value::ofInt(1), Value::ofVarchar("\xC3\xA9")},
                                         {Value::ofInt(1), Value::ofVarchar("a")},
                                         {Value::ofInt(1), Value::ofVarchar("B")}};
    CHECK(run(plan) == expected);
}

TEST_CASE("sort: an unknown sort key is rejected while building, before any page is read")
{
    TestDb db("sort_unknown_key");
    db.createTable("R", rSchema(), rRows(10));
    db.coldPool();
    ExecContext ctx(db.engine());
    const storage::IoStats before = db.engine().pool().stats();
    try
    {
        buildExecutablePlan(physicalFromJsonText(R"({"op": "sort", "keys": [["R.zz", "asc"]],
                                                     "child": {"op": "seqscan", "table": "R"}})"),
                            ctx);
        FAIL("expected db::Error");
    }
    catch (const Error& e)
    {
        CHECK(e.category() == ErrorCategory::Name);
    }
    CHECK(db.engine().pool().stats() == before);
}

TEST_CASE("sort: expectedSortPasses = 1 + ceil(log(ceil(N/B)) / log(B-1)), in whole numbers")
{
    CHECK(expectedSortPasses(8, 3) == 3);
    CHECK(expectedSortPasses(80, 4) == 4);
    CHECK(expectedSortPasses(9, 4) == 2); // ceil(9/4) = 3 runs, one merge
    CHECK(expectedSortPasses(1000, 10) == 4); // 100 -> 12 -> 2 -> 1
    // ceil(108/4) = 27 runs: 27 -> 9 -> 3 -> 1. With floating logs,
    // log(27)/log(3) can come out a hair above 3, and its ceiling 4 gives 5.
    CHECK(expectedSortPasses(108, 4) == 4);
    CHECK(expectedSortPasses(4, 4) == 1); // one run
    CHECK(expectedSortPasses(0, 4) == 1); // empty input
}
