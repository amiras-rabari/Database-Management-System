// CSV loader (chunk 13): good files, quoted fields, CRLF, missing final
// newline, every line/column error, rows before a bad line kept with the
// catalog counting them, record too long, 241 rows = 2 pages, 500 rows that
// survive a reopen, loadCsvFile and its missing-file error.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/csv_loader.h"
#include "db/storage/heap_file.h"
#include "db/storage/heap_scan.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/rid.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"

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

    // R(a INT, b VARCHAR(5), c INT): the chunk guide's table.
    Schema schemaR()
    {
        return Schema({Column{"", "a", Type::Int, 0}, Column{"", "b", Type::Varchar, 5},
            Column{"", "c", Type::Int, 0}});
    }

    // An engine on `dir` that has table R.
    std::unique_ptr<StorageEngine> engineWithR(const fs::path& dir, std::size_t frames = 8)
    {
        auto engine = StorageEngine::open(dir, frames, ReplacementPolicy::Lru);
        engine->catalog().createTable("R", schemaR());
        return engine;
    }

    LoadResult loadText(StorageEngine& engine, const std::string& table, const std::string& text)
    {
        std::istringstream in(text);
        return loadCsv(engine, table, in);
    }

    // Every row of `table`, in scan order.
    std::vector<Tuple> scanAll(StorageEngine& engine, const std::string& table)
    {
        std::vector<Tuple> rows;
        HeapScan scan = engine.openHeapFile(table).scan();
        Rid rid;
        Tuple row;
        while (scan.next(rid, row))
            rows.push_back(row);
        return rows;
    }

    Tuple rowR(std::int32_t a, const std::string& b, std::int32_t c)
    {
        return {Value::ofInt(a), Value::ofVarchar(b), Value::ofInt(c)};
    }

    // The error loadText throws (fails the test if none).
    Error loadError(StorageEngine& engine, const std::string& table, const std::string& text)
    {
        try
        {
            loadText(engine, table, text);
        }
        catch (const Error& e)
        {
            return e;
        }
        FAIL("no error for: " << text);
        return Error(ErrorCategory::Syntax, "unreachable");
    }

    bool contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }
} // namespace

TEST_CASE("loader: two good lines load in order and the catalog counts them")
{
    TempDir dir("loader_good");
    auto engine = engineWithR(dir.path());
    const LoadResult result = loadText(*engine, "R", "42,bob,7\n43,amy,9\n");
    CHECK(result.rows == 2);
    CHECK(scanAll(*engine, "R") == std::vector<Tuple>{rowR(42, "bob", 7), rowR(43, "amy", 9)});
    const TableInfo info = *engine->catalog().getTable("R");
    CHECK(info.numTuples == 2);
    CHECK(info.numPages == 1);
    engine->close();
}

TEST_CASE("loader: quoted fields, with and without a comma")
{
    TempDir dir("loader_quoted");
    auto engine = engineWithR(dir.path());
    loadText(*engine, "R", "1,\"a,b\",2\n1,\"bob\",2\n3,\"x\"\"y\",4\n");
    CHECK(scanAll(*engine, "R") == std::vector<Tuple>{rowR(1, "a,b", 2), rowR(1, "bob", 2), rowR(3, "x\"y", 4)});
    engine->close();
}

TEST_CASE("loader: CRLF and a missing final newline load the same as LF")
{
    TempDir dir("loader_crlf");
    auto engine = engineWithR(dir.path());
    engine->catalog().createTable("S", schemaR());
    engine->catalog().createTable("T", schemaR());
    loadText(*engine, "R", "42,bob,7\n43,amy,9\n");
    loadText(*engine, "S", "42,bob,7\r\n43,amy,9\r\n");
    loadText(*engine, "T", "42,bob,7\n43,amy,9");
    CHECK(scanAll(*engine, "S") == scanAll(*engine, "R"));
    CHECK(scanAll(*engine, "T") == scanAll(*engine, "R"));
    CHECK(engine->catalog().getTable("T")->numTuples == 2);
    engine->close();
}

TEST_CASE("loader: an empty input loads nothing and changes nothing")
{
    TempDir dir("loader_empty");
    auto engine = engineWithR(dir.path());
    CHECK(loadText(*engine, "R", "").rows == 0);
    CHECK(engine->catalog().getTable("R")->numTuples == 0);
    CHECK(scanAll(*engine, "R").empty());
    engine->close();
}

TEST_CASE("loader: bad INT on line 3 stops the load; lines 1-2 stay and are counted")
{
    TempDir dir("loader_bad_int");
    {
        auto engine = engineWithR(dir.path());
        const Error e = loadError(*engine, "R", "42,bob,7\n43,amy,9\nx,bob,7\n44,eve,1\n");
        CHECK(e.category() == ErrorCategory::Type);
        CHECK(e.line() == 3);
        CHECK(e.column() == 1);
        CHECK(contains(e.message(), "line 3, column 1 (a): "));
        CHECK(scanAll(*engine, "R") == std::vector<Tuple>{rowR(42, "bob", 7), rowR(43, "amy", 9)});
        const TableInfo info = *engine->catalog().getTable("R");
        CHECK(info.numTuples == 2);
        CHECK(info.numPages == 1);
        engine->close();
    }
    // The kept rows and their counts survive a reopen.
    auto engine = StorageEngine::open(dir.path(), 8, ReplacementPolicy::Lru);
    CHECK(engine->catalog().getTable("R")->numTuples == 2);
    CHECK(scanAll(*engine, "R").size() == 2);
    engine->close();
}

TEST_CASE("loader: spaces are data, so ' 7' is a bad INT")
{
    TempDir dir("loader_spaces");
    auto engine = engineWithR(dir.path());
    const Error e = loadError(*engine, "R", "1, bob, 7\n");
    CHECK(e.category() == ErrorCategory::Type);
    CHECK(e.column() == 3);
    CHECK(engine->catalog().getTable("R")->numTuples == 0);
    engine->close();
}

TEST_CASE("loader: VARCHAR longer than n on line 2 names column 2")
{
    TempDir dir("loader_long_varchar");
    auto engine = engineWithR(dir.path());
    const Error e = loadError(*engine, "R", "1,abcde,2\n1,abcdef,2\n");
    CHECK(e.category() == ErrorCategory::Schema);
    CHECK(e.line() == 2);
    CHECK(e.column() == 2);
    CHECK(contains(e.message(), "line 2, column 2 (b): "));
    CHECK(engine->catalog().getTable("R")->numTuples == 1);
    engine->close();
}

TEST_CASE("loader: wrong field counts name the first missing or extra field")
{
    TempDir dir("loader_field_count");
    auto engine = engineWithR(dir.path());

    Error e = loadError(*engine, "R", "1,bob\n");
    CHECK(e.category() == ErrorCategory::Schema);
    CHECK(e.line() == 1);
    CHECK(e.column() == 3);
    CHECK(contains(e.message(), "line 1, column 3 (c): expected 3 fields, found 2"));

    e = loadError(*engine, "R", "1,bob,2,3,4\n");
    CHECK(e.column() == 4);
    CHECK(contains(e.message(), "line 1, column 4: expected 3 fields, found 5"));

    e = loadError(*engine, "R", "1\n");
    CHECK(e.column() == 2);
    engine->close();
}

TEST_CASE("loader: a blank line is an error (wrong field count), also with CRLF")
{
    TempDir dir("loader_blank");
    auto engine = engineWithR(dir.path());
    Error e = loadError(*engine, "R", "1,bob,2\n\n3,amy,4\n");
    CHECK(e.category() == ErrorCategory::Schema);
    CHECK(e.line() == 2);
    CHECK(e.column() == 1);
    CHECK(contains(e.message(), "found a blank line"));
    CHECK(engine->catalog().getTable("R")->numTuples == 1);

    e = loadError(*engine, "R", "\r\n");
    CHECK(e.line() == 1);
    CHECK(e.column() == 1);
    engine->close();
}

TEST_CASE("loader: splitting errors carry the line and the field's column")
{
    TempDir dir("loader_split_errors");
    auto engine = engineWithR(dir.path());
    Error e = loadError(*engine, "R", "5,ok,5\n1,\"bob,2\n");
    CHECK(e.category() == ErrorCategory::Syntax);
    CHECK(e.line() == 2);
    CHECK(e.column() == 2);
    CHECK(contains(e.message(), "line 2, column 2 (b): unterminated quoted field"));

    e = loadError(*engine, "R", "1,\"bo\"b,2\n");
    CHECK(e.line() == 1);
    CHECK(e.column() == 2);
    CHECK(contains(e.message(), "text after the closing quote"));
    engine->close();
}

TEST_CASE("loader: a record too long for a page names the table and the line; one that just fits loads")
{
    TempDir dir("loader_too_long");
    auto engine = StorageEngine::open(dir.path(), 8, ReplacementPolicy::Lru);
    // 16 VARCHAR(255) columns. 15 full fields = 15 * 257 = 3855 bytes; a
    // last field of 227 bytes makes 3855 + 229 = 4084 (the most that fits,
    // D5), and 228 bytes makes 4085.
    std::vector<Column> columns;
    for (int i = 0; i < 16; ++i)
        columns.push_back(Column{"", "v" + std::to_string(i), Type::Varchar, 255});
    engine->catalog().createTable("Wide", Schema(columns));

    std::string fits;
    for (int i = 0; i < 15; ++i)
        fits += std::string(255, 'x') + ",";
    std::string tooLong = fits + std::string(228, 'y');
    fits += std::string(227, 'y');

    const Error e = loadError(*engine, "Wide", fits + "\n" + tooLong + "\n");
    CHECK(e.category() == ErrorCategory::Schema);
    CHECK(e.line() == 2);
    CHECK(contains(e.message(), "line 2"));
    CHECK(contains(e.message(), "'Wide'"));
    CHECK(contains(e.message(), "record too long (4085 bytes"));
    CHECK(engine->catalog().getTable("Wide")->numTuples == 1);
    CHECK(engine->catalog().getTable("Wide")->numPages == 1);
    engine->close();
}

TEST_CASE("loader: unknown table is Error(Name) and reads nothing")
{
    TempDir dir("loader_unknown");
    auto engine = StorageEngine::open(dir.path(), 8, ReplacementPolicy::Lru);
    const Error e = loadError(*engine, "Nope", "1,2,3\n");
    CHECK(e.category() == ErrorCategory::Name);
    CHECK(e.message() == "Unknown table 'Nope'.");
    engine->close();
}

TEST_CASE("loader: 241 rows of 13 bytes need 2 data pages (capacity 240)")
{
    TempDir dir("loader_capacity");
    auto engine = engineWithR(dir.path());
    std::string text;
    for (int i = 0; i < 241; ++i)
        text += std::to_string(i) + ",abc," + std::to_string(i) + "\n"; // 4 + 5 + 4 = 13 bytes
    CHECK(loadText(*engine, "R", text).rows == 241);
    const TableInfo info = *engine->catalog().getTable("R");
    CHECK(info.numPages == 2);
    CHECK(info.numTuples == 241);
    engine->close();
}

TEST_CASE("loader: two loads add up; 500 rows survive a reopen in order")
{
    TempDir dir("loader_500");
    {
        auto engine = engineWithR(dir.path(), 4);
        std::string first;
        std::string second;
        for (int i = 0; i < 500; ++i)
            (i < 300 ? first : second) += std::to_string(i) + ",r" + std::to_string(i % 1000) + "," + std::to_string(-i)
                + "\n";
        CHECK(loadText(*engine, "R", first).rows == 300);
        CHECK(loadText(*engine, "R", second).rows == 200);
        CHECK(engine->catalog().getTable("R")->numTuples == 500);
        engine->close();
    }
    auto engine = StorageEngine::open(dir.path(), 4, ReplacementPolicy::Lru);
    const TableInfo info = *engine->catalog().getTable("R");
    CHECK(info.numTuples == 500);
    const std::vector<Tuple> rows = scanAll(*engine, "R");
    REQUIRE(rows.size() == 500);
    for (int i = 0; i < 500; ++i)
        CHECK(rows[static_cast<std::size_t>(i)] == rowR(i, "r" + std::to_string(i), -i));
    CHECK(info.numPages == engine->openHeapFile("R").numDataPages());
    engine->close();
}

TEST_CASE("loader: loadCsvFile reads a binary CRLF file; a missing file names its path")
{
    TempDir dir("loader_file");
    const fs::path csv = dir.path() / "r.csv";
    {
        std::ofstream out(csv, std::ios::binary);
        out << "42,bob,7\r\n43,amy,9\r\n";
    }
    auto engine = engineWithR(dir.path() / "db");
    CHECK(loadCsvFile(*engine, "R", csv).rows == 2);
    CHECK(scanAll(*engine, "R") == std::vector<Tuple>{rowR(42, "bob", 7), rowR(43, "amy", 9)});

    const fs::path missing = dir.path() / "missing.csv";
    try
    {
        loadCsvFile(*engine, "R", missing);
        FAIL("no error");
    }
    catch (const Error& e)
    {
        CHECK(e.category() == ErrorCategory::Name);
        CHECK(contains(e.message(), missing.string()));
    }
    CHECK(engine->catalog().getTable("R")->numTuples == 2);
    engine->close();
}
