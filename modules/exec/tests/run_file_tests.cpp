// CHUNK 06 tests: writing tuples to a temporary run file and reading them
// back. Every full page is written at once and a finished run's pages are
// dropped from the pool (Q3 (a)), so a run of N pages costs exactly N writes
// and N reads, as in the spec's sort table (§M2.3).

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/io_stats.h"
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
    // Writes `rows` to a new run and returns it. Checks that no more than one
    // page is pinned while writing.
    storage::TempFile writeRun(TestDb& db, const Schema& schema, const std::vector<Tuple>& rows)
    {
        RunWriter writer(db.engine(), schema);
        for (const Tuple& row : rows)
        {
            writer.append(row);
            REQUIRE(db.engine().pool().pinnedFrameCount() <= 1);
        }
        return writer.finish();
    }

    // Reads the whole run. Checks that no more than one page is pinned.
    std::vector<Tuple> readRun(TestDb& db, const storage::TempFile& run, const Schema& schema)
    {
        RunReader reader(db.engine(), run, schema);
        std::vector<Tuple> rows;
        Tuple t;
        while (reader.next(t))
        {
            rows.push_back(t);
            REQUIRE(db.engine().pool().pinnedFrameCount() <= 1);
        }
        return rows;
    }
} // namespace

TEST_CASE("run file: 2400 tuples of 13 bytes make 10 pages, written once and read once")
{
    TestDb db("run_ten_pages");
    const Schema schema = rSchema();

    const storage::IoStats beforeWrite = db.engine().pool().stats();
    const storage::TempFile run = writeRun(db, schema, rRows(2400));
    const storage::IoStats written = db.engine().pool().stats() - beforeWrite;
    CHECK(run.numPages() == 10); // 240 tuples per page (storage golden)
    CHECK(written.writes == 10);
    CHECK(written.reads == 0);
    CHECK(db.engine().pool().pinnedFrameCount() == 0);

    // The finished run's pages were dropped from the pool, so every page is
    // read from disk: 10 reads, not hits.
    const storage::IoStats beforeRead = db.engine().pool().stats();
    CHECK(readRun(db, run, schema) == rRows(2400));
    const storage::IoStats read = db.engine().pool().stats() - beforeRead;
    CHECK(read.reads == 10);
    CHECK(read.writes == 0);
    CHECK(db.engine().pool().pinnedFrameCount() == 0);
}

TEST_CASE("run file: a run small enough to stay in the pool is still read from disk")
{
    // 2 pages with B = 4: without dropping the finished run's pages, both
    // would still be cached and the read would cost 0 instead of 2.
    TestDb db("run_small");
    const Schema schema = rSchema();
    const storage::TempFile run = writeRun(db, schema, rRows(480));
    CHECK(run.numPages() == 2);

    const storage::IoStats before = db.engine().pool().stats();
    CHECK(readRun(db, run, schema) == rRows(480));
    CHECK((db.engine().pool().stats() - before).reads == 2);
}

TEST_CASE("run file: destroying the run deletes its temporary file")
{
    TestDb db("run_delete");
    const std::size_t before = db.engine().liveTempFileCount();
    {
        const storage::TempFile run = writeRun(db, rSchema(), rRows(500));
        CHECK(db.engine().liveTempFileCount() == before + 1);
    }
    CHECK(db.engine().liveTempFileCount() == before);
}

TEST_CASE("run file: an empty run has no pages and costs no I/O")
{
    TestDb db("run_empty");
    const Schema schema = rSchema();
    const storage::IoStats before = db.engine().pool().stats();
    const storage::TempFile run = writeRun(db, schema, {});
    CHECK(run.numPages() == 0);
    CHECK(readRun(db, run, schema).empty());
    CHECK(db.engine().pool().stats() == before);
}

TEST_CASE("run file: a tuple too long for one page is a schema error, and nothing is left behind")
{
    // 20 columns of VARCHAR(255): a full tuple is 20 * (2 + 255) = 5140
    // bytes, more than the 4084 a page can hold (Q1 (b)).
    std::vector<Column> columns;
    for (int i = 0; i < 20; ++i)
        columns.push_back({"W", "c" + std::to_string(i), Type::Varchar, 255});
    const Schema wide(columns);
    const Tuple small(20, Value::ofVarchar("x")); // 20 * 3 = 60 bytes: fits
    const Tuple big(20, Value::ofVarchar(std::string(255, 'y')));

    TestDb db("run_too_long");
    const std::size_t tempFilesBefore = db.engine().liveTempFileCount();
    try
    {
        RunWriter writer(db.engine(), wide);
        writer.append(small);
        writer.append(big);
        FAIL("expected db::Error");
    }
    catch (const Error& e)
    {
        CHECK(e.category() == ErrorCategory::Schema);
    }
    CHECK(db.engine().pool().pinnedFrameCount() == 0);
    CHECK(db.engine().liveTempFileCount() == tempFilesBefore);
}
