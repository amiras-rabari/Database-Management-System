// Catalog and StorageEngine (chunk 12): create / get / list tables, counts and
// statistics that survive a reopen, rejected names, no I/O for lookups (S8,
// D4), close / resetPool refusals, a catalog spread over several pages that
// later shrinks, and byte-identical catalog.db files.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/bytes.h"
#include "db/storage/catalog.h"
#include "db/storage/heap_file.h"
#include "db/storage/heap_scan.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/page_guard.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/rid.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"
#include "db/storage/temp_file.h"

namespace
{
    namespace fs = std::filesystem;
    using namespace db;
    using namespace db::storage;

    // A fresh folder under the system temp directory, removed when the test
    // starts and again when it ends. Each test case uses its own name.
    class TempDir
    {
    public:
        explicit TempDir(const std::string& name)
            : path_(fs::temp_directory_path() / "comp3005_storage_tests" / name)
        {
            fs::remove_all(path_);
            fs::create_directories(path_);
        }
        ~TempDir()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
        const fs::path& path() const { return path_; }

    private:
        fs::path path_;
    };

    std::unique_ptr<StorageEngine> openEngine(const fs::path& dir, std::size_t frames = 8)
    {
        return StorageEngine::open(dir, frames, ReplacementPolicy::Lru);
    }

    // R(a INT, b VARCHAR(20), c INT), bare.
    Schema schemaR()
    {
        return Schema({Column{"", "a", Type::Int, 0}, Column{"", "b", Type::Varchar, 20},
            Column{"", "c", Type::Int, 0}});
    }

    Schema oneInt()
    {
        return Schema({Column{"", "a", Type::Int, 0}});
    }

    TableStats statsR()
    {
        TableStats stats{500, 3, 12.75, {}};
        ColumnStats a{500, Value::ofInt(1), Value::ofInt(500), {}};
        a.histogram = {{Value::ofInt(1), Value::ofInt(250), 250, 250}, {Value::ofInt(251), Value::ofInt(500), 250, 250}};
        ColumnStats b{2, Value::ofVarchar("cs"), Value::ofVarchar("math"), {}};
        b.histogram = {{Value::ofVarchar("cs"), Value::ofVarchar("math"), 500, 2}};
        ColumnStats c{1, Value::ofInt(0), Value::ofInt(0), {}};
        stats.columns = {a, b, c};
        return stats;
    }

    // Statistics for a one-INT-column table with `buckets` buckets.
    TableStats intStats(int buckets)
    {
        TableStats stats{static_cast<std::uint64_t>(buckets) * 10, 1, 4.0, {}};
        ColumnStats a{static_cast<std::uint64_t>(buckets), Value::ofInt(0), Value::ofInt(buckets * 10), {}};
        for (int i = 0; i < buckets; ++i)
            a.histogram.push_back({Value::ofInt(i * 10), Value::ofInt(i * 10 + 9), 10, 10});
        stats.columns = {a};
        return stats;
    }

    // The category of the db::Error that `action` throws (fails if none).
    ErrorCategory categoryOf(const std::function<void()>& action)
    {
        try
        {
            action();
        }
        catch (const Error& error)
        {
            return error.category();
        }
        FAIL("no db::Error was thrown");
        return ErrorCategory::Lexical;
    }

    // The message of the db::Error that `action` throws.
    std::string messageOf(const std::function<void()>& action)
    {
        try
        {
            action();
        }
        catch (const Error& error)
        {
            return error.message();
        }
        return "(no db::Error)";
    }

    std::vector<char> fileBytes(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    Tuple rowR(int i)
    {
        return {Value::ofInt(i), Value::ofVarchar(i % 2 == 0 ? "cs" : "math"), Value::ofInt(i * 3)};
    }
} // namespace

TEST_CASE("a new folder gets an empty catalog.db and no tables")
{
    TempDir dir("catalog_new");
    const fs::path db = dir.path() / "newdb"; // does not exist yet
    {
        auto engine = openEngine(db);
        CHECK(fs::exists(db / "catalog.db"));
        CHECK(engine->catalog().listTables().empty());
        CHECK(engine->pool().stats() == IoStats{});
        engine->close();
    }
    // Nothing changed, so nothing was saved: the file still has no pages.
    CHECK(fs::file_size(db / "catalog.db") == 0);
    auto again = openEngine(db);
    CHECK(again->catalog().listTables().empty());
}

TEST_CASE("createTable makes R.db with its directory page and a qualified getTable")
{
    TempDir dir("catalog_create");
    auto engine = openEngine(dir.path());
    engine->catalog().createTable("R", schemaR());
    CHECK(fs::file_size(dir.path() / "R.db") == kPageSize);

    const std::optional<TableInfo> info = engine->catalog().getTable("R");
    REQUIRE(info.has_value());
    CHECK(info->name == "R");
    CHECK(info->dataFile == "R.db");
    CHECK(info->firstPage == 0);
    CHECK(info->numPages == 0);
    CHECK(info->numTuples == 0);
    CHECK_FALSE(info->stats.has_value());
    REQUIRE(info->schema.size() == 3);
    CHECK(info->schema.column(0).qualifiedName() == "R.a");
    CHECK(info->schema.column(1).qualifiedName() == "R.b");
    CHECK(info->schema.column(2).qualifiedName() == "R.c");
    CHECK(info->schema.column(1).type == Type::Varchar);
    CHECK(info->schema.column(1).varcharLength == 20);
    CHECK(engine->catalog().hasTable("R"));

    // A qualified schema (as sql's CREATE TABLE might pass) is stored bare.
    engine->catalog().createTable("S", Schema({Column{"X", "k", Type::Int, 0}}));
    CHECK(engine->catalog().getTable("S")->schema.column(0).qualifiedName() == "S.k");

    // The heap file is usable at once.
    HeapFile heap = engine->openHeapFile("R");
    heap.insert(rowR(1));
    CHECK(heap.schema().column(0).qualifiedName() == "R.a");
    CHECK(heap.numDataPages() == 1);
}

TEST_CASE("unknown tables: getTable is empty, the others throw Error(Name)")
{
    TempDir dir("catalog_unknown");
    auto engine = openEngine(dir.path());
    engine->catalog().createTable("R", schemaR());
    CHECK_FALSE(engine->catalog().getTable("S").has_value());
    CHECK_FALSE(engine->catalog().hasTable("S"));
    // Lookups are exact: with only R, "r" does not exist.
    CHECK_FALSE(engine->catalog().getTable("r").has_value());

    CHECK(categoryOf([&] { engine->openHeapFile("S"); }) == ErrorCategory::Name);
    CHECK(messageOf([&] { engine->openHeapFile("S"); }) == "Unknown table 'S'.");
    CHECK(categoryOf([&] { engine->catalog().updateCounts("S", 1, 1); }) == ErrorCategory::Name);
    CHECK(categoryOf([&] { engine->catalog().setStats("S", statsR()); }) == ErrorCategory::Name);
    CHECK(messageOf([&] { engine->catalog().updateCounts("S", 1, 1); }) == "Unknown table 'S'.");
}

TEST_CASE("tables and counts survive close and reopen; listTables is sorted by byte order")
{
    TempDir dir("catalog_persist");
    {
        auto engine = openEngine(dir.path());
        engine->catalog().createTable("S", oneInt());
        engine->catalog().createTable("R", schemaR());
        engine->catalog().updateCounts("R", 3, 500);
        engine->close();
    }
    auto engine = openEngine(dir.path());
    CHECK(engine->catalog().listTables() == std::vector<std::string>{"R", "S"});
    const TableInfo r = *engine->catalog().getTable("R");
    CHECK(r.numPages == 3);
    CHECK(r.numTuples == 500);
    CHECK(r.dataFile == "R.db");
    CHECK(sameColumns(r.schema, requalify(schemaR(), "R")));
    CHECK(engine->catalog().getTable("S")->numTuples == 0);

    engine->catalog().createTable("dept", oneInt());
    engine->catalog().createTable("Emp", oneInt());
    CHECK(engine->catalog().listTables() == std::vector<std::string>{"Emp", "R", "S", "dept"});
}

TEST_CASE("an updateCounts alone (in a later session) is saved")
{
    // createTable already marks the catalog changed, so the count change
    // must happen in a session of its own to prove updateCounts marks it too.
    TempDir dir("catalog_counts_only");
    {
        auto engine = openEngine(dir.path());
        engine->catalog().createTable("R", schemaR());
        engine->close();
    }
    {
        auto engine = openEngine(dir.path());
        CHECK_FALSE(engine->catalog().changed());
        engine->catalog().updateCounts("R", 7, 1234);
        CHECK(engine->catalog().changed());
        engine->close();
    }
    const TableInfo r = *openEngine(dir.path())->catalog().getTable("R");
    CHECK(r.numPages == 7);
    CHECK(r.numTuples == 1234);
}

TEST_CASE("the destructor saves too (close not called)")
{
    TempDir dir("catalog_destructor");
    {
        auto engine = openEngine(dir.path());
        engine->catalog().createTable("R", schemaR());
    }
    CHECK(openEngine(dir.path())->catalog().hasTable("R"));
}

TEST_CASE("statistics survive a reopen; setStats checks them against the schema")
{
    TempDir dir("catalog_stats");
    {
        auto engine = openEngine(dir.path());
        engine->catalog().createTable("R", schemaR());
        engine->catalog().setStats("R", statsR());
        CHECK(engine->catalog().getTable("R")->stats == statsR());

        TableStats tooFew = statsR();
        tooFew.columns.pop_back();
        CHECK_THROWS_AS(engine->catalog().setStats("R", tooFew), std::logic_error);
        TableStats wrongType = statsR();
        wrongType.columns[1].min = Value::ofInt(3);
        CHECK_THROWS_AS(engine->catalog().setStats("R", wrongType), std::logic_error);
        // A rejected setStats keeps the old statistics.
        CHECK(engine->catalog().getTable("R")->stats == statsR());
        engine->close();
    }
    auto engine = openEngine(dir.path());
    const TableInfo r = *engine->catalog().getTable("R");
    REQUIRE(r.stats.has_value());
    CHECK(*r.stats == statsR());
    CHECK(r.stats->width == 12.75);
}

TEST_CASE("bad table names and schemas are rejected with Error(Schema) and change nothing")
{
    TempDir dir("catalog_reject");
    auto engine = openEngine(dir.path());
    Catalog& catalog = engine->catalog();
    catalog.createTable("R", schemaR());

    for (const std::string name : {"catalog", "Catalog", "../x", "a b", "", "R-1", "x/y", "t.db"})
    {
        CAPTURE(name);
        CHECK(categoryOf([&] { catalog.createTable(name, oneInt()); }) == ErrorCategory::Schema);
    }
    CHECK(messageOf([&] { catalog.createTable("a b", oneInt()); })
          == "Invalid table name 'a b' (use only letters, digits and _).");

    CHECK(categoryOf([&] { catalog.createTable("R", oneInt()); }) == ErrorCategory::Schema);
    CHECK(messageOf([&] { catalog.createTable("R", oneInt()); }) == "Table 'R' already exists.");

    // No columns; two columns with one name once the qualifiers are gone.
    CHECK(categoryOf([&] { catalog.createTable("E", Schema()); }) == ErrorCategory::Schema);
    CHECK(categoryOf([&] {
        catalog.createTable("D", Schema({Column{"R", "a", Type::Int, 0}, Column{"S", "a", Type::Int, 0}}));
    }) == ErrorCategory::Schema);

    // VARCHAR(0) and VARCHAR(256) can never reach createTable: the Schema
    // itself refuses them.
    CHECK(categoryOf([] { Schema({Column{"", "v", Type::Varchar, 0}}); }) == ErrorCategory::Schema);
    CHECK(categoryOf([] { Schema({Column{"", "v", Type::Varchar, 256}}); }) == ErrorCategory::Schema);

    CHECK(catalog.listTables() == std::vector<std::string>{"R"});
    CHECK_FALSE(fs::exists(dir.path() / "E.db"));
    CHECK_FALSE(fs::exists(dir.path() / "D.db"));
    CHECK(fs::file_size(dir.path() / "catalog.db") == 0); // nothing saved yet
}

TEST_CASE("names that differ only in case are different tables (sql SPEC: Emp and emp)")
{
    TempDir dir("catalog_case");
    auto engine = openEngine(dir.path());
    Catalog& catalog = engine->catalog();
    catalog.createTable("Emp", oneInt());

    // On a case-sensitive file system (Linux) emp.db is a new file; on a
    // case-insensitive one (Windows) it is Emp.db, which already exists.
    const bool caseInsensitiveFiles = fs::exists(dir.path() / "emp.db");
    if (caseInsensitiveFiles)
    {
        // Fails safely: the registry never overwrites a file; nothing changes.
        CHECK(categoryOf([&] { catalog.createTable("emp", oneInt()); }) == ErrorCategory::Name);
        CHECK(catalog.listTables() == std::vector<std::string>{"Emp"});
    }
    else
    {
        catalog.createTable("emp", oneInt());
        CHECK(catalog.listTables() == std::vector<std::string>{"Emp", "emp"});
        CHECK(fs::exists(dir.path() / "Emp.db"));
        CHECK(fs::exists(dir.path() / "emp.db"));
    }
    // Lookups are exact in both cases.
    CHECK(catalog.getTable("Emp").has_value());
    CHECK(catalog.getTable("EMP") == std::nullopt);
}

TEST_CASE("S8: getTable costs no I/O; open reads each catalog page once")
{
    TempDir dir("catalog_s8");
    {
        auto engine = openEngine(dir.path());
        engine->catalog().createTable("R", schemaR());
        engine->catalog().setStats("R", statsR());
        engine->close();
    }
    auto engine = openEngine(dir.path());
    // One catalog page, read once through the pool at open (D4).
    CHECK(engine->pool().stats().reads == 1);
    const IoStats before = engine->pool().stats();
    for (int i = 0; i < 10; ++i)
        CHECK(engine->catalog().getTable("R")->stats == statsR());
    CHECK(engine->catalog().hasTable("R"));
    CHECK(engine->catalog().listTables().size() == 1);
    CHECK(engine->pool().stats() == before);
}

TEST_CASE("openHeapFile twice gives the same file")
{
    TempDir dir("catalog_open_twice");
    auto engine = openEngine(dir.path());
    engine->catalog().createTable("R", schemaR());
    CHECK(engine->openHeapFile("R").fileId() == engine->openHeapFile("R").fileId());
}

TEST_CASE("close refuses while a page is pinned or a temp file is alive; then works, twice")
{
    TempDir dir("catalog_close");
    auto engine = openEngine(dir.path());
    engine->catalog().createTable("R", schemaR());
    {
        HeapFile heap = engine->openHeapFile("R");
        PageGuard guard = PageGuard::fetch(engine->pool(), heap.fileId(), 0);
        CHECK_THROWS_AS(engine->close(), std::logic_error);
    }
    {
        TempFile temp = engine->createTempFile();
        CHECK(engine->liveTempFileCount() == 1);
        CHECK_THROWS_AS(engine->close(), std::logic_error);
    }
    CHECK(engine->liveTempFileCount() == 0);
    engine->close();
    engine->close(); // idempotent
    CHECK(fs::file_size(dir.path() / "catalog.db") == kPageSize);
}

TEST_CASE("resetPool: new frame count, zero counters, tables kept, refusals")
{
    TempDir dir("catalog_reset");
    auto engine = openEngine(dir.path(), 4);
    engine->catalog().createTable("R", schemaR());
    HeapFile(engine->openHeapFile("R")).insert(rowR(1));
    CHECK(engine->pool().stats() != IoStats{});

    engine->resetPool(8);
    CHECK(engine->pool().frameCount() == 8);
    CHECK(engine->pool().stats() == IoStats{});
    CHECK(engine->pool().pinnedFrameCount() == 0);
    CHECK(engine->catalog().hasTable("R"));

    // The table and the new pool work together (the old page reads back).
    HeapFile heap = engine->openHeapFile("R");
    CHECK(heap.get(Rid{1, 0}) == rowR(1));
    CHECK(engine->pool().stats().reads == 1); // data page 1, from disk

    // The temp manager uses the new pool.
    {
        TempFile temp = engine->createTempFile();
        PageGuard page = PageGuard::newPage(engine->pool(), temp.fileId());
        CHECK(temp.numPages() == 1);
        CHECK(engine->pool().pinnedFrameCount() == 1);
        CHECK_THROWS_AS(engine->resetPool(4), std::logic_error); // pinned and alive
        page.release();
        CHECK_THROWS_AS(engine->resetPool(4), std::logic_error); // temp file alive
    }

    {
        PageGuard guard = PageGuard::fetch(engine->pool(), heap.fileId(), 0);
        CHECK_THROWS_AS(engine->resetPool(4), std::logic_error);
        CHECK(engine->pool().frameCount() == 8);
    }

    // Zero frames: refused, the old pool stays.
    CHECK_THROWS_AS(engine->resetPool(0), std::logic_error);
    CHECK(engine->pool().frameCount() == 8);

    // A table created before a reset is saved by it (flushAll) and survives.
    engine->catalog().createTable("S", oneInt());
    engine->resetPool(2);
    CHECK(engine->pool().frameCount() == 2);
    engine->close();
    CHECK(openEngine(dir.path())->catalog().listTables() == std::vector<std::string>{"R", "S"});
}

TEST_CASE("500 rows: insert, updateCounts, reopen, scan")
{
    TempDir dir("catalog_500");
    {
        auto engine = openEngine(dir.path(), 4);
        engine->catalog().createTable("R", schemaR());
        HeapFile heap = engine->openHeapFile("R");
        for (int i = 0; i < 500; ++i)
            heap.insert(rowR(i));
        engine->catalog().updateCounts("R", heap.numDataPages(), 500);
        engine->close();
    }
    auto engine = openEngine(dir.path(), 4);
    const TableInfo info = *engine->catalog().getTable("R");
    CHECK(info.numTuples == 500);
    HeapFile heap = engine->openHeapFile("R");
    CHECK(info.numPages == heap.numDataPages());
    CHECK(info.numPages > 1);

    HeapScan scan = heap.scan();
    Rid rid;
    Tuple row;
    int count = 0;
    while (scan.next(rid, row))
    {
        CHECK(row == rowR(count));
        ++count;
    }
    CHECK(count == 500);
}

TEST_CASE("the same steps in two folders give byte-identical catalog.db files")
{
    TempDir dir("catalog_determinism");
    auto build = [](const fs::path& db) {
        auto engine = openEngine(db);
        engine->catalog().createTable("S", oneInt());
        engine->catalog().createTable("R", schemaR());
        engine->catalog().updateCounts("R", 3, 500);
        engine->catalog().setStats("R", statsR());
        engine->catalog().setStats("S", intStats(300));
        engine->close();
    };
    build(dir.path() / "one");
    build(dir.path() / "two");
    const std::vector<char> one = fileBytes(dir.path() / "one" / "catalog.db");
    const std::vector<char> two = fileBytes(dir.path() / "two" / "catalog.db");
    CHECK(one.size() >= 2 * kPageSize);
    CHECK(one == two);
}

TEST_CASE("many tables need two catalog pages and all come back")
{
    TempDir dir("catalog_many");
    std::vector<std::string> names;
    {
        auto engine = openEngine(dir.path(), 2);
        for (int i = 0; i < 120; ++i)
        {
            std::string name = "T" + std::to_string(1000 + i);
            engine->catalog().createTable(name, oneInt());
            engine->catalog().updateCounts(name, static_cast<std::uint32_t>(i), static_cast<std::uint64_t>(i) * 7);
            names.push_back(name);
        }
        engine->close();
    }
    CHECK(fs::file_size(dir.path() / "catalog.db") == 2 * kPageSize);
    auto engine = openEngine(dir.path(), 2);
    CHECK(engine->catalog().listTables() == names);
    CHECK(engine->catalog().getTable("T1119")->numTuples == 119 * 7);
    CHECK(engine->pool().stats().reads == 2);
}

TEST_CASE("a catalog that shrinks leaves empty pages behind and reloads correctly")
{
    TempDir dir("catalog_shrink");
    const fs::path catalogPath = dir.path() / "catalog.db";
    {
        // 400 INT buckets = 9 640 bytes of statistics = 3 chunks, each on its
        // own page after the table record's page: 4 pages.
        auto engine = openEngine(dir.path(), 2);
        engine->catalog().createTable("Big", oneInt());
        engine->catalog().setStats("Big", intStats(400));
        engine->close();
    }
    CHECK(fs::file_size(catalogPath) == 4 * kPageSize);
    {
        auto engine = openEngine(dir.path(), 2);
        CHECK(engine->catalog().getTable("Big")->stats == intStats(400));
        engine->catalog().setStats("Big", intStats(1));
        engine->close();
    }
    // The file does not shrink; pages 1..3 are now empty slotted pages.
    CHECK(fs::file_size(catalogPath) == 4 * kPageSize);
    std::vector<char> raw = fileBytes(catalogPath);
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        bytes[i] = static_cast<std::byte>(raw[i]);
    const std::span<const std::byte> all(bytes);
    CHECK(readU16(all, 4) == 2); // page 0: table record + the one small chunk
    for (std::size_t p = 1; p < 4; ++p)
    {
        CAPTURE(p);
        CHECK(readU32(all, p * kPageSize) == p);         // page_id
        CHECK(readU16(all, p * kPageSize + 4) == 0);     // slot_count
        CHECK(readU16(all, p * kPageSize + 6) == 4096);  // free_end
    }

    auto engine = openEngine(dir.path(), 2);
    CHECK(engine->catalog().getTable("Big")->stats == intStats(1));
    CHECK(engine->catalog().listTables() == std::vector<std::string>{"Big"});
}

TEST_CASE("a corrupt catalog.db is reported with its page and slot")
{
    TempDir dir("catalog_corrupt");
    {
        auto engine = openEngine(dir.path());
        engine->catalog().createTable("T", oneInt());
        engine->close();
    }
    // Page 0 has one record, T(a INT), 34 bytes at the end of the page.
    // Its type tag (record byte 31) becomes 7.
    {
        std::fstream file(dir.path() / "catalog.db", std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(kPageSize - 34 + 31));
        file.put(static_cast<char>(7));
    }
    CHECK_THROWS_WITH_AS(openEngine(dir.path()), doctest::Contains("catalog.db page 0 slot 0"),
                         std::runtime_error);
}
