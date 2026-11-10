// Chunk 03: ANALYZE on real tables in a fresh database folder.
// Data is synthetic (the M4 workload is not available yet).

#include <doctest/doctest.h>

#include <db/common/error.h>
#include <db/common/schema.h>
#include <db/common/value.h>
#include <db/opt/analyze.h>
#include <db/storage/buffer_pool.h>
#include <db/storage/catalog.h>
#include <db/storage/heap_file.h>
#include <db/storage/io_stats.h>
#include <db/storage/replacement_policy.h>
#include <db/storage/storage_engine.h>
#include <db/storage/table_info.h>
#include <db/storage/table_stats.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using db::Column;
    using db::Schema;
    using db::Type;
    using db::Value;
    using db::storage::HistogramBucket;
    using db::storage::StorageEngine;
    using db::storage::TableInfo;
    using db::storage::TableStats;

    // An empty database folder of its own for each test.
    std::filesystem::path freshDir(const std::string& name)
    {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opt_analyze_" + name);
        std::filesystem::remove_all(dir);
        return dir;
    }

    // T(a INT, b VARCHAR(10)) with `rows` rows: a = i mod 100, b = "v" + (i mod 7).
    // The catalog counts are updated the way the loader does it.
    std::unique_ptr<StorageEngine> openWithTable(const std::filesystem::path& dir, std::size_t frames,
                                                 std::uint64_t rows)
    {
        std::unique_ptr<StorageEngine> engine =
            StorageEngine::open(dir, frames, db::storage::ReplacementPolicy::Lru);
        engine->catalog().createTable("T", Schema({{"T", "a", Type::Int, 0}, {"T", "b", Type::Varchar, 10}}));
        db::storage::HeapFile heap = engine->openHeapFile("T");
        for (std::uint64_t i = 0; i < rows; ++i)
        {
            heap.insert({Value::ofInt(static_cast<std::int32_t>(i % 100)),
                         Value::ofVarchar("v" + std::to_string(i % 7))});
        }
        engine->catalog().updateCounts("T", heap.numDataPages(), rows);
        return engine;
    }

    // ANALYZE on a pool that was just emptied, so its counters show exactly
    // the I/O of this one run (no pages left cached by an earlier step).
    db::storage::IoStats analyzeCold(StorageEngine& engine, std::size_t frames, std::int32_t buckets)
    {
        engine.resetPool(frames);
        db::opt::analyzeTable(engine, "T", buckets);
        return engine.pool().stats();
    }
} // namespace

TEST_CASE("ANALYZE: exact numbers for a 1,000-row table, stored in the catalog")
{
    std::filesystem::path dir = freshDir("basic");
    std::unique_ptr<StorageEngine> engine = openWithTable(dir, 32, 1000);

    TableStats stats = db::opt::analyzeTable(*engine, "T", 20);

    CHECK(stats.nTuples == 1000);
    // n_pages is the catalog's data page count (§M4.1: directory pages excluded).
    CHECK(stats.nPages == engine->catalog().getTable("T")->numPages);

    // Width = average record length: 4 bytes for a, 2 + length for b.
    // Every b is "v" plus one digit, so each record is 4 + 2 + 2 = 8 bytes.
    std::uint64_t totalBytes = 0;
    for (std::uint64_t i = 0; i < 1000; ++i)
    {
        totalBytes += 4 + 2 + ("v" + std::to_string(i % 7)).size();
    }
    CHECK(stats.width == static_cast<double>(totalBytes) / 1000.0);

    // Column a: 0..99, ten copies each -> 20 buckets of 50 rows, 5 values
    // (the same buckets as chunk 02's "runs aligned with cuts" test).
    REQUIRE(stats.columns.size() == 2);
    CHECK(stats.columns[0].ndv == 100);
    CHECK(stats.columns[0].min == Value::ofInt(0));
    CHECK(stats.columns[0].max == Value::ofInt(99));
    std::vector<HistogramBucket> expected;
    for (std::int32_t i = 0; i < 20; ++i)
    {
        expected.push_back({Value::ofInt(5 * i), Value::ofInt(5 * i + 4), 50, 5});
    }
    CHECK(stats.columns[0].histogram == expected);

    // Column b: "v0" .. "v6".
    CHECK(stats.columns[1].ndv == 7);
    CHECK(stats.columns[1].min == Value::ofVarchar("v0"));
    CHECK(stats.columns[1].max == Value::ofVarchar("v6"));

    // The same statistics are now in the catalog.
    std::optional<TableInfo> info = engine->catalog().getTable("T");
    REQUIRE(info->stats.has_value());
    CHECK(*info->stats == stats);
    engine->close();
}

TEST_CASE("ANALYZE scans the table once when B has room for every column's sort")
{
    // B = 32: two columns need 2 x 2 frames + 1 for the scan, so one scan
    // feeds both sorts. Reads = the table's data pages + its 1 directory page
    // (one scan, not two), + column b's run: b's sort writes the values it
    // holds as a run (so column a can use every frame) and reads it back.
    // A one-column b record is "vN" = 2 + 2 = 4 bytes plus a 4-byte slot, so
    // 4088 / 8 = 511 fit on a page and 1,000 values take 2 run pages. Column
    // a's values stay in memory: 0 writes for it.
    std::filesystem::path dir = freshDir("once");
    std::unique_ptr<StorageEngine> engine = openWithTable(dir, 32, 1000);
    std::uint64_t dataPages = engine->catalog().getTable("T")->numPages;
    const std::uint64_t runPagesOfB = (1000 + 511 - 1) / 511;

    db::storage::IoStats io = analyzeCold(*engine, 32, 20);

    CHECK(io.reads == dataPages + 1 + runPagesOfB);
    CHECK(io.writes == runPagesOfB);
    engine->close();
}

TEST_CASE("ANALYZE twice changes nothing")
{
    std::filesystem::path dir = freshDir("twice");
    std::unique_ptr<StorageEngine> engine = openWithTable(dir, 32, 1000);
    TableInfo before = *engine->catalog().getTable("T");
    std::uint32_t filePages = engine->openHeapFile("T").numDataPages();

    db::storage::IoStats firstIo = analyzeCold(*engine, 32, 20);
    TableStats first = *engine->catalog().getTable("T")->stats;
    db::storage::IoStats secondIo = analyzeCold(*engine, 32, 20);
    TableStats second = *engine->catalog().getTable("T")->stats;

    CHECK(first == second);
    CHECK(firstIo == secondIo);
    // ANALYZE only reads the table: counts and data pages are unchanged.
    TableInfo after = *engine->catalog().getTable("T");
    CHECK(after.numTuples == before.numTuples);
    CHECK(after.numPages == before.numPages);
    CHECK(engine->openHeapFile("T").numDataPages() == filePages);
    engine->close();
}

TEST_CASE("ANALYZE at B = 4 on a 20+ page table: same stats, at most 4 frames, no temp files")
{
    // 10,000 rows of 8 bytes is about 30 data pages, so each column sort
    // needs several runs at B = 4. B = 4 fits one sort per scan, B = 5 two:
    // the statistics must not depend on how the columns were grouped.
    std::filesystem::path dir = freshDir("small_b");
    std::unique_ptr<StorageEngine> engine = openWithTable(dir, 32, 10000);
    REQUIRE(engine->catalog().getTable("T")->numPages >= 20);

    TableStats atB32 = db::opt::analyzeTable(*engine, "T", 20);

    engine->resetPool(5);
    TableStats atB5 = db::opt::analyzeTable(*engine, "T", 20);
    CHECK(atB5 == atB32);
    CHECK(engine->pool().peakPinnedCount() <= 5);

    engine->resetPool(4);
    TableStats atB4 = db::opt::analyzeTable(*engine, "T", 20);
    CHECK(atB4 == atB32);
    CHECK(engine->pool().peakPinnedCount() <= 4);
    CHECK(engine->liveTempFileCount() == 0);
    CHECK(engine->pool().pinnedFrameCount() == 0);
    engine->close();
}

TEST_CASE("ANALYZE of an unknown table is a name error and changes nothing")
{
    std::filesystem::path dir = freshDir("unknown");
    std::unique_ptr<StorageEngine> engine = openWithTable(dir, 32, 10);

    try
    {
        db::opt::analyzeTable(*engine, "Nope", 20);
        FAIL("expected Error(Name)");
    }
    catch (const db::Error& error)
    {
        CHECK(error.category() == db::ErrorCategory::Name);
    }
    CHECK_FALSE(engine->catalog().getTable("T")->stats.has_value());
    CHECK(engine->catalog().listTables() == std::vector<std::string>{"T"});
    engine->close();
}

TEST_CASE("ANALYZE of an empty table: ndv 0, no buckets, schema width")
{
    std::filesystem::path dir = freshDir("empty");
    std::unique_ptr<StorageEngine> engine = openWithTable(dir, 32, 0);

    TableStats stats = db::opt::analyzeTable(*engine, "T", 20);

    CHECK(stats.nTuples == 0);
    // No rows to average: the widest record the schema allows, 4 + (2 + 10).
    CHECK(stats.width == 16.0);
    for (const db::storage::ColumnStats& column : stats.columns)
    {
        CHECK(column.ndv == 0);
        CHECK(column.histogram.empty());
    }
    CHECK(engine->catalog().getTable("T")->stats == stats);
    engine->close();
}

TEST_CASE("ANALYZE with 5 buckets gives at most 5 buckets per column")
{
    std::filesystem::path dir = freshDir("buckets");
    std::unique_ptr<StorageEngine> engine = openWithTable(dir, 32, 1000);

    TableStats stats = db::opt::analyzeTable(*engine, "T", 5);

    CHECK(stats.columns[0].histogram.size() == 5);
    CHECK(stats.columns[1].histogram.size() <= 5);
    engine->close();
}
