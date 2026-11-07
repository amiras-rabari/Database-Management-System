// CHUNK 08 tests: sort-based distinct, union, intersect and except
// (SPEC §M2.3). Choices tested here (exec STATUS.md, chunk 08):
// - set semantics: every output tuple appears once (C20);
// - union feeds both inputs into ONE sorter (Q1 (a));
// - intersect and except sort each input to one run on disk with
//   ExternalSorter::finishToRun, then merge the two runs (Q2 (a)).
// Tables are one INT column x unless a test says otherwise. A record is
// 4 bytes, so a page holds floor(4088 / (4 + 4)) = 511 rows.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "db/common/error.h"
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
#include "db/storage/temp_file.h"
#include "sort/run_file.h"
#include "tests/test_db.h"

using namespace db;
using namespace db::exec;
using db::exec::test::rRows;
using db::exec::test::rSchema;
using db::exec::test::TestDb;

namespace
{
    // Table `name` with one INT column x and these values, in this order.
    void createIntTable(TestDb& db, const std::string& name, const std::vector<int>& values)
    {
        std::vector<Tuple> rows;
        for (int v : values)
            rows.push_back({Value::ofInt(v)});
        db.createTable(name, Schema({{name, "x", Type::Int, 0}}), rows);
    }

    // Builds the plan, runs it to the end and closes it (close checks that no
    // page is pinned and no temp file is left).
    std::vector<Tuple> runPlan(TestDb& db, const std::string& json)
    {
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(json), ctx);
        std::vector<Tuple> rows;
        plan.open();
        Tuple t;
        while (plan.next(t))
            rows.push_back(t);
        plan.close();
        return rows;
    }

    // The single INT of each row, in output order.
    std::vector<int> ints(const std::vector<Tuple>& rows)
    {
        std::vector<int> values;
        for (const Tuple& row : rows)
            values.push_back(row[0].asInt());
        return values;
    }

    std::string scan(const std::string& table)
    {
        return R"({"op": "seqscan", "table": ")" + table + R"("})";
    }

    std::string twoInputs(const std::string& op, const std::string& left, const std::string& right)
    {
        return R"({"op": ")" + op + R"(", "left": )" + scan(left) + R"(, "right": )" + scan(right) + "}";
    }

    std::string distinctOf(const std::string& table)
    {
        return R"({"op": "distinct", "mode": "sort", "child": )" + scan(table) + "}";
    }
} // namespace

TEST_CASE("set ops: finishToRun leaves exactly one sorted run on disk and holds no frame")
{
    // The spec's 8 pages, with the frames a root sort gets at B = 4: runs of
    // 3 pages, merges of fan-in 3.
    TestDb db("setops_finish_to_run");
    db.createTable("R", rSchema(), rRows(1920)); // 8 data pages
    db.coldPool();
    const Schema schema = rSchema();
    std::uint64_t comparisons = 0;
    {
        ExternalSorter sorter(db.engine(), schema, {SortKey{ColumnRef{"R", "a"}, false}}, 3, 4, 4, comparisons);
        storage::HeapFile heap = db.engine().openHeapFile("R");
        storage::HeapScan scan = heap.scan();
        storage::Rid rid;
        Tuple t;
        while (scan.next(rid, t))
            sorter.add(t);
        scan.close();
        const storage::IoStats start = db.engine().pool().stats();

        // Pass 0 already wrote runs 1 and 2 (6 pages); finishToRun writes run
        // 3 (2 pages), then one merge pass turns the 3 runs into 1 (8 reads,
        // 8 writes). Nothing is left open on it.
        storage::TempFile run = sorter.finishToRun();
        CHECK(sorter.stats().initialRuns == 3);
        CHECK(sorter.stats().passes == 2);
        CHECK(run.numPages() == 8);
        CHECK((db.engine().pool().stats() - start).reads == 8);
        CHECK((db.engine().pool().stats() - start).writes == 2 + 8);
        CHECK(db.engine().pool().pinnedFrameCount() == 0);
        CHECK(db.engine().liveTempFileCount() == 1);

        // Reading it back gives every row, largest a first: 8 more reads.
        std::vector<Tuple> rows;
        {
            RunReader reader(db.engine(), run, schema);
            while (reader.next(t))
                rows.push_back(t);
        }
        std::vector<Tuple> expected = rRows(1920);
        std::reverse(expected.begin(), expected.end());
        CHECK(rows == expected);
        CHECK((db.engine().pool().stats() - start).reads == 16);
    }
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("set ops: finishToRun keeps merging until one run is left (fan-in 2)")
{
    // All frames 3: runs 3, 3, 2 -> pass 1 gives 2 runs -> pass 2 gives 1.
    // Every pass writes all 8 pages. (Losing a run would not show in a test
    // full of duplicates, so this one checks the page count and every row.)
    TestDb db("setops_finish_two_passes");
    db.createTable("R", rSchema(), rRows(1920));
    db.coldPool();
    const Schema schema = rSchema();
    std::uint64_t comparisons = 0;
    ExternalSorter sorter(db.engine(), schema, {SortKey{ColumnRef{"R", "a"}, true}}, 3, 3, 3, comparisons);
    {
        storage::HeapFile heap = db.engine().openHeapFile("R");
        storage::HeapScan scan = heap.scan();
        storage::Rid rid;
        Tuple t;
        while (scan.next(rid, t))
            sorter.add(t);
        scan.close();
    }
    storage::TempFile run = sorter.finishToRun();
    CHECK(sorter.stats().passes == 3);
    CHECK(run.numPages() == 8);
    CHECK(db.engine().pool().stats().writes == 8 + 8 + 8);
    std::vector<Tuple> rows;
    {
        RunReader reader(db.engine(), run, schema);
        Tuple t;
        while (reader.next(t))
            rows.push_back(t);
    }
    CHECK(rows == rRows(1920));
}

TEST_CASE("set ops: finishToRun on an empty input gives an empty run")
{
    TestDb db("setops_finish_empty");
    const Schema schema = rSchema();
    std::uint64_t comparisons = 0;
    ExternalSorter sorter(db.engine(), schema, {SortKey{ColumnRef{"R", "a"}, true}}, 3, 4, 4, comparisons);
    storage::TempFile run = sorter.finishToRun();
    CHECK(run.numPages() == 0);
    CHECK(sorter.stats().initialRuns == 0);
    CHECK(db.engine().pool().stats().writes == 0);
}

TEST_CASE("set ops: distinct (sort mode) drops adjacent equal tuples after sorting")
{
    TestDb db("setops_distinct");
    createIntTable(db, "A", {3, 1, 3, 2, 1, 3});
    ExecContext ctx(db.engine());
    ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(distinctOf("A")), ctx);
    std::vector<Tuple> rows;
    plan.open();
    Tuple t;
    while (plan.next(t))
        rows.push_back(t);
    plan.close();
    CHECK(ints(rows) == std::vector<int>{1, 2, 3});
    CHECK(plan.trace()[0].counters.tuplesOut == 3);
    CHECK(plan.trace()[0].counters.comparisons > 0);
    CHECK(plan.schema().column(0).qualifiedName() == "A.x");
}

TEST_CASE("set ops: union removes duplicates within and across the inputs; names come from the left")
{
    TestDb db("setops_union");
    createIntTable(db, "A", {1, 2, 2});
    createIntTable(db, "B", {2, 3});
    ExecContext ctx(db.engine());
    ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(twoInputs("union", "A", "B")), ctx);
    CHECK(plan.schema().column(0).qualifiedName() == "A.x");
    std::vector<Tuple> rows;
    plan.open();
    Tuple t;
    while (plan.next(t))
        rows.push_back(t);
    plan.close();
    CHECK(ints(rows) == std::vector<int>{1, 2, 3});
}

TEST_CASE("set ops: intersect outputs each value on both sides once")
{
    TestDb db("setops_intersect");
    createIntTable(db, "A", {1, 1, 2, 3});
    createIntTable(db, "B", {1, 3, 3, 4});
    CHECK(ints(runPlan(db, twoInputs("intersect", "A", "B"))) == std::vector<int>{1, 3});
    // Order of the inputs does not matter for the result.
    CHECK(ints(runPlan(db, twoInputs("intersect", "B", "A"))) == std::vector<int>{1, 3});
}

TEST_CASE("set ops: except outputs each left value missing on the right once")
{
    TestDb db("setops_except");
    createIntTable(db, "A", {1, 1, 2, 3});
    createIntTable(db, "B", {3});
    createIntTable(db, "C", {1, 2});
    createIntTable(db, "E", {});
    createIntTable(db, "F", {1});
    CHECK(ints(runPlan(db, twoInputs("except", "A", "B"))) == std::vector<int>{1, 2});
    CHECK(ints(runPlan(db, twoInputs("except", "C", "E"))) == std::vector<int>{1, 2});
    CHECK(ints(runPlan(db, twoInputs("except", "E", "F"))).empty());
    // Right side larger than everything on the left: nothing survives.
    CHECK(ints(runPlan(db, twoInputs("except", "F", "C"))).empty());
}

TEST_CASE("set ops: empty inputs through every operator")
{
    TestDb db("setops_empty");
    createIntTable(db, "E", {});
    createIntTable(db, "A", {2, 1, 2});
    CHECK(runPlan(db, distinctOf("E")).empty());
    CHECK(runPlan(db, twoInputs("union", "E", "E")).empty());
    CHECK(ints(runPlan(db, twoInputs("union", "E", "A"))) == std::vector<int>{1, 2});
    CHECK(ints(runPlan(db, twoInputs("union", "A", "E"))) == std::vector<int>{1, 2});
    CHECK(runPlan(db, twoInputs("intersect", "E", "A")).empty());
    CHECK(runPlan(db, twoInputs("intersect", "A", "E")).empty());
    CHECK(runPlan(db, twoInputs("except", "E", "E")).empty());
    CHECK(db.engine().liveTempFileCount() == 0);
}

TEST_CASE("set ops: inputs of about 30 pages each with many duplicates, at B = 4")
{
    // A: 15330 rows = 30 pages, values 0..2999 (each about 5 times).
    // B: 15330 rows = 30 pages, values 2000..5999 (each about 4 times).
    TestDb db("setops_large");
    std::vector<int> a;
    std::vector<int> b;
    for (int i = 0; i < 15330; ++i)
    {
        a.push_back((i * 7) % 3000);
        b.push_back(2000 + (i * 11) % 4000);
    }
    createIntTable(db, "A", a);
    createIntTable(db, "B", b);

    // The reference results, worked out in the test with std::set (outside
    // the plan, so not counted against B).
    const std::set<int> setA(a.begin(), a.end());
    const std::set<int> setB(b.begin(), b.end());
    std::vector<int> expectUnion;
    std::vector<int> expectIntersect;
    std::vector<int> expectExcept;
    std::set_union(setA.begin(), setA.end(), setB.begin(), setB.end(), std::back_inserter(expectUnion));
    std::set_intersection(setA.begin(), setA.end(), setB.begin(), setB.end(), std::back_inserter(expectIntersect));
    std::set_difference(setA.begin(), setA.end(), setB.begin(), setB.end(), std::back_inserter(expectExcept));
    const std::vector<int> expectDistinct(setA.begin(), setA.end());

    struct Case
    {
        std::string json;
        std::vector<int> expected;
    };
    const std::vector<Case> cases = {
        {distinctOf("A"), expectDistinct},
        {twoInputs("union", "A", "B"), expectUnion},
        {twoInputs("intersect", "A", "B"), expectIntersect},
        {twoInputs("except", "A", "B"), expectExcept},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.json);
        db.coldPool();
        ExecContext ctx(db.engine());
        ExecutablePlan plan = buildExecutablePlan(physicalFromJsonText(c.json), ctx);
        std::vector<Tuple> rows;
        plan.open();
        Tuple t;
        while (plan.next(t))
            rows.push_back(t);
        plan.close();
        CHECK(ints(rows) == c.expected);
        CHECK(plan.maxFramesHeld() <= 4);
        CHECK(plan.totals().pageWrites > 0); // it really spilled to disk
        CHECK(db.engine().liveTempFileCount() == 0);
    }
}

TEST_CASE("set ops: incompatible inputs are rejected while building")
{
    TestDb db("setops_incompatible");
    createIntTable(db, "A", {1});
    db.createTable("V", Schema({{"V", "s", Type::Varchar, 5}}), {{Value::ofVarchar("a")}});
    db.createTable("W", Schema({{"W", "x", Type::Int, 0}, {"W", "y", Type::Int, 0}}),
                   {{Value::ofInt(1), Value::ofInt(2)}});
    db.coldPool();
    for (const std::string op : {"union", "intersect", "except"})
    {
        CAPTURE(op);
        ExecContext ctx(db.engine());
        CHECK_THROWS_AS(buildExecutablePlan(physicalFromJsonText(twoInputs(op, "A", "V")), ctx), Error);
        CHECK_THROWS_AS(buildExecutablePlan(physicalFromJsonText(twoInputs(op, "A", "W")), ctx), Error);
        try
        {
            buildExecutablePlan(physicalFromJsonText(twoInputs(op, "A", "W")), ctx);
        }
        catch (const Error& e)
        {
            CHECK(e.category() == ErrorCategory::Schema);
        }
    }
    CHECK(db.engine().pool().stats().reads == 0);
}

TEST_CASE("set ops: VARCHAR values compare byte-wise; union takes a longer right VARCHAR")
{
    TestDb db("setops_varchar");
    // L.s is VARCHAR(3), M.s is VARCHAR(10): compatible (lengths may differ).
    db.createTable("L", Schema({{"L", "s", Type::Varchar, 3}}),
                   {{Value::ofVarchar("a")}, {Value::ofVarchar("A")}, {Value::ofVarchar("b")}, {Value::ofVarchar("a")}});
    db.createTable("M", Schema({{"M", "s", Type::Varchar, 10}}),
                   {{Value::ofVarchar("a")}, {Value::ofVarchar("longer")}});

    std::vector<std::string> got;
    for (const Tuple& row : runPlan(db, twoInputs("union", "L", "M")))
        got.push_back(row[0].asVarchar());
    // Byte order: 'A' (65) < 'a' (97) < 'b' < 'l'.
    CHECK(got == std::vector<std::string>{"A", "a", "b", "longer"});

    got.clear();
    for (const Tuple& row : runPlan(db, twoInputs("intersect", "L", "M")))
        got.push_back(row[0].asVarchar());
    CHECK(got == std::vector<std::string>{"a"}); // "A" is not "a"

    got.clear();
    for (const Tuple& row : runPlan(db, twoInputs("except", "L", "M")))
        got.push_back(row[0].asVarchar());
    CHECK(got == std::vector<std::string>{"A", "b"});
}
