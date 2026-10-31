// Catalog record format (chunk 11, "Version 2 (Simple)"): table records
// byte by byte, the stats blob, cutting it into chunks of at most 4084
// bytes and joining them back, and corrupt / truncated records.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "catalog/catalog_format.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace
{
    using namespace db;
    using namespace db::storage;
    namespace format = db::storage::catalog_format;

    // The bytes as upper-case hex pairs, so a failing check is readable.
    std::string hex(std::span<const std::byte> bytes)
    {
        const char* digits = "0123456789ABCDEF";
        std::string text;
        for (std::byte b : bytes)
        {
            if (!text.empty())
                text += ' ';
            auto value = std::to_integer<unsigned>(b);
            text += digits[value >> 4];
            text += digits[value & 0xF];
        }
        return text;
    }

    TableInfo tableR()
    {
        TableInfo info;
        info.name = "R";
        info.schema = Schema({Column{"", "a", Type::Int, 0}, Column{"", "b", Type::Varchar, 20},
            Column{"", "c", Type::Int, 0}});
        info.dataFile = "R.db";
        info.firstPage = 0;
        info.numPages = 3;
        info.numTuples = 500;
        return info;
    }

    // Statistics for R(a INT, b VARCHAR(20), c INT), 3 buckets on a and b.
    TableStats statsR()
    {
        TableStats stats;
        stats.nTuples = 500;
        stats.nPages = 3;
        stats.width = 1.0 / 3.0 + 20.0; // not a round number: bits must survive
        ColumnStats a{500, Value::ofInt(-2147483647 - 1), Value::ofInt(2147483647), {}};
        a.histogram = {{Value::ofInt(-2147483647 - 1), Value::ofInt(0), 200, 150},
            {Value::ofInt(1), Value::ofInt(100), 200, 100}, {Value::ofInt(101), Value::ofInt(2147483647), 100, 100}};
        ColumnStats b{3, Value::ofVarchar(""), Value::ofVarchar("zzzzzzzzzzzzzzzzzzzz"), {}};
        b.histogram = {{Value::ofVarchar(""), Value::ofVarchar("cs"), 100, 2},
            {Value::ofVarchar("ee"), Value::ofVarchar("math"), 300, 1},
            {Value::ofVarchar("o'neil"), Value::ofVarchar("zzzzzzzzzzzzzzzzzzzz"), 100, 1}};
        ColumnStats c{1, Value::ofInt(7), Value::ofInt(7), {}};
        stats.columns = {a, b, c};
        return stats;
    }

    std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values)
    {
        std::vector<std::byte> out;
        for (unsigned v : values)
            out.push_back(static_cast<std::byte>(v));
        return out;
    }
} // namespace

TEST_CASE("table record of R(a INT, b VARCHAR(20), c INT) round-trips with bare names")
{
    const TableInfo info = tableR();
    const std::vector<std::byte> record = format::encodeTableRecord(info);
    CHECK(format::recordKind(record) == format::kTableRecord);
    const TableInfo back = format::decodeTableRecord(record);
    CHECK(back == info);
    CHECK(back.schema.column(1).name == "b");
    CHECK(back.schema.column(1).qualifier.empty());
    CHECK(back.schema.column(1).varcharLength == 20);
    CHECK_FALSE(back.stats.has_value());
}

TEST_CASE("table record of T(a INT) has the exact bytes of DESIGN_M1.md")
{
    TableInfo info;
    info.name = "T";
    info.schema = Schema({Column{"", "a", Type::Int, 0}});
    info.dataFile = "T.db";
    const std::vector<std::byte> expected = bytesOf({
        0x01,                               // kind 1 = table record
        0x01, 0x00, 0x54,                   // name "T"
        0x04, 0x00, 0x54, 0x2E, 0x64, 0x62, // data file "T.db"
        0x00, 0x00, 0x00, 0x00,             // first page 0
        0x00, 0x00, 0x00, 0x00,             // num pages 0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // num tuples 0 (u64)
        0x01, 0x00,                         // 1 column
        0x01, 0x00, 0x61, 0x00, 0x00, 0x00, // "a", INT tag 0, length 0
    });
    CHECK(hex(format::encodeTableRecord(info)) == hex(expected));
    CHECK(format::encodeTableRecord(info).size() == 34);
}

TEST_CASE("a qualified schema is stored bare")
{
    TableInfo info = tableR();
    info.schema = Schema({Column{"R", "a", Type::Int, 0}, Column{"R", "b", Type::Varchar, 20},
        Column{"R", "c", Type::Int, 0}});
    CHECK(format::decodeTableRecord(format::encodeTableRecord(info)) == tableR());
}

TEST_CASE("a truncated or padded table record is rejected as corrupt")
{
    std::vector<std::byte> record = format::encodeTableRecord(tableR());
    std::vector<std::byte> shorter(record.begin(), record.end() - 1);
    CHECK_THROWS_WITH_AS(format::decodeTableRecord(shorter), doctest::Contains("cut short"), std::runtime_error);

    // Every shorter prefix fails too (each length field is checked).
    for (std::size_t n = 0; n < record.size(); ++n)
        CHECK_THROWS_AS(format::decodeTableRecord(std::span(record).first(n)), std::runtime_error);

    std::vector<std::byte> longer = record;
    longer.push_back(std::byte{0});
    CHECK_THROWS_WITH_AS(format::decodeTableRecord(longer), doctest::Contains("left over"), std::runtime_error);

    // A name length that claims more bytes than the record has.
    std::vector<std::byte> badLength = record;
    badLength[1] = std::byte{0xFF};
    CHECK_THROWS_AS(format::decodeTableRecord(badLength), std::runtime_error);
}

TEST_CASE("impossible values in a table record are rejected as corrupt")
{
    // T(a INT): byte 31 is the type tag, bytes 32..33 the VARCHAR length.
    TableInfo info;
    info.name = "T";
    info.schema = Schema({Column{"", "a", Type::Int, 0}});
    info.dataFile = "T.db";
    const std::vector<std::byte> good = format::encodeTableRecord(info);

    std::vector<std::byte> badTag = good;
    badTag[31] = std::byte{7};
    CHECK_THROWS_WITH_AS(format::decodeTableRecord(badTag), doctest::Contains("type tag"), std::runtime_error);

    std::vector<std::byte> varchar0 = good; // VARCHAR(0)
    varchar0[31] = std::byte{1};
    CHECK_THROWS_WITH_AS(format::decodeTableRecord(varchar0), doctest::Contains("impossible schema"),
                         std::runtime_error);

    std::vector<std::byte> wrongKind = good;
    wrongKind[0] = std::byte{2};
    CHECK_THROWS_AS(format::decodeTableRecord(wrongKind), std::runtime_error);

    CHECK_THROWS_AS(format::recordKind({}), std::runtime_error);
}

TEST_CASE("stats blob round-trips exactly (INT and VARCHAR bounds, width bits)")
{
    const Schema schema = tableR().schema;
    const TableStats stats = statsR();
    const std::vector<std::byte> blob = format::encodeStats(stats, schema);
    const TableStats back = format::decodeStats(blob, schema);
    CHECK(back == stats);
    CHECK(back.width == stats.width);
    CHECK(back.columns[1].histogram[2].lo == Value::ofVarchar("o'neil"));
    CHECK(back.columns[0].min == Value::ofInt(-2147483647 - 1));
    CHECK(back.columns[2].histogram.empty());

    // The blob starts with nTuples = 500 (u64) and nPages = 3 (u32).
    CHECK(hex(std::span(blob).first(12)) == "F4 01 00 00 00 00 00 00 03 00 00 00");

    // Truncated by one byte, or one byte too many: corrupt.
    CHECK_THROWS_AS(format::decodeStats(std::span(blob).first(blob.size() - 1), schema), std::runtime_error);
    std::vector<std::byte> longer = blob;
    longer.push_back(std::byte{0});
    CHECK_THROWS_AS(format::decodeStats(longer, schema), std::runtime_error);
}

TEST_CASE("stats that do not match the schema cannot be encoded")
{
    const Schema schema = tableR().schema;
    TableStats tooFew = statsR();
    tooFew.columns.pop_back();
    CHECK_THROWS_AS(format::encodeStats(tooFew, schema), std::logic_error);

    TableStats wrongType = statsR();
    wrongType.columns[0].max = Value::ofVarchar("x"); // a is INT
    CHECK_THROWS_AS(format::encodeStats(wrongType, schema), std::logic_error);

    TableStats wrongBucket = statsR();
    wrongBucket.columns[1].histogram[0].hi = Value::ofInt(1); // b is VARCHAR
    CHECK_THROWS_AS(format::encodeStats(wrongBucket, schema), std::logic_error);

    // A blob decoded with a schema of another column count is corrupt.
    const std::vector<std::byte> blob = format::encodeStats(statsR(), schema);
    CHECK_THROWS_AS(format::decodeStats(blob, Schema({Column{"", "a", Type::Int, 0}})), std::runtime_error);
}

TEST_CASE("cutting into chunks: every record at most 4084 bytes, join gives the blob back")
{
    const std::string name = "R";
    const std::size_t capacity = format::chunkPayloadCapacity(name);
    CHECK(capacity == 4084 - (1 + 2 + 1 + 2 + 2));

    // Sizes around the chunk boundary: 0, 1, capacity - 1, capacity,
    // capacity + 1, 3 * capacity + 5.
    const std::vector<std::size_t> sizes = {0, 1, capacity - 1, capacity, capacity + 1, 3 * capacity + 5};
    const std::vector<std::size_t> expectedChunks = {1, 1, 1, 1, 2, 4};
    for (std::size_t k = 0; k < sizes.size(); ++k)
    {
        CAPTURE(sizes[k]);
        std::vector<std::byte> blob(sizes[k]);
        for (std::size_t i = 0; i < blob.size(); ++i)
            blob[i] = static_cast<std::byte>(i * 7 + 3);

        const auto records = format::cutIntoChunks(name, blob);
        REQUIRE(records.size() == expectedChunks[k]);
        std::vector<format::StatsChunk> chunks;
        for (std::size_t i = 0; i < records.size(); ++i)
        {
            CHECK(records[i].size() <= format::kMaxCatalogRecord);
            CHECK(format::recordKind(records[i]) == format::kStatsChunk);
            chunks.push_back(format::decodeStatsChunk(records[i]));
            CHECK(chunks.back().tableName == name);
            CHECK(chunks.back().chunkNumber == i);
            CHECK(chunks.back().chunkCount == records.size());
        }
        // Every chunk but the last is completely full (exactly 4084 bytes).
        for (std::size_t i = 0; i + 1 < records.size(); ++i)
            CHECK(records[i].size() == format::kMaxCatalogRecord);
        CHECK(format::joinChunks(chunks) == blob);
    }

    // A blob of exactly `capacity` bytes gives one record of exactly 4084 bytes.
    std::vector<std::byte> full(capacity);
    CHECK(format::cutIntoChunks(name, full).front().size() == format::kMaxCatalogRecord);
}

TEST_CASE("a missing, extra or out-of-order chunk is rejected")
{
    std::vector<std::byte> blob(10000, std::byte{0x5A});
    std::vector<format::StatsChunk> chunks;
    for (const auto& record : format::cutIntoChunks("R", blob))
        chunks.push_back(format::decodeStatsChunk(record));
    REQUIRE(chunks.size() == 3);
    CHECK(format::joinChunks(chunks) == blob);

    std::vector<format::StatsChunk> missing = {chunks[0], chunks[2]};
    CHECK_THROWS_AS(format::joinChunks(missing), std::runtime_error);

    std::vector<format::StatsChunk> swapped = {chunks[0], chunks[2], chunks[1]};
    CHECK_THROWS_WITH_AS(format::joinChunks(swapped), doctest::Contains("out of order"), std::runtime_error);

    std::vector<format::StatsChunk> extra = {chunks[0], chunks[1], chunks[2], chunks[2]};
    CHECK_THROWS_AS(format::joinChunks(extra), std::runtime_error);

    CHECK_THROWS_AS(format::joinChunks({}), std::runtime_error);

    // A chunk record cut inside its header, or numbered past its count.
    const auto records = format::cutIntoChunks("R", blob);
    CHECK_THROWS_AS(format::decodeStatsChunk(std::span(records[0]).first(6)), std::runtime_error);
    std::vector<std::byte> badNumber = records[0];
    badNumber[4] = std::byte{3}; // chunk number 3 of 3
    CHECK_THROWS_AS(format::decodeStatsChunk(badNumber), std::runtime_error);
}

TEST_CASE("an INT column with 400 buckets needs 3 chunks and survives the trip")
{
    const Schema schema({Column{"", "a", Type::Int, 0}});
    TableStats stats{100000, 400, 4.0, {}};
    ColumnStats a{100000, Value::ofInt(0), Value::ofInt(99999), {}};
    for (int i = 0; i < 400; ++i)
        a.histogram.push_back({Value::ofInt(i * 250), Value::ofInt(i * 250 + 249), 250, 250});
    stats.columns = {a};

    const std::vector<std::byte> blob = format::encodeStats(stats, schema);
    // 22 header bytes + ndv 8 + min 4 + max 4 + count 2 + 400 × (4 + 4 + 8 + 8).
    CHECK(blob.size() == 22 + 18 + 400 * 24);
    const auto records = format::cutIntoChunks("Big", blob);
    CHECK(records.size() == 3);
    std::vector<format::StatsChunk> chunks;
    for (const auto& record : records)
    {
        CHECK(record.size() <= format::kMaxCatalogRecord);
        chunks.push_back(format::decodeStatsChunk(record));
    }
    CHECK(format::decodeStats(format::joinChunks(chunks), schema) == stats);
}

TEST_CASE("VARCHAR(255) with 20 buckets of 255-byte bounds is split, never written past a page")
{
    // One bucket: (2 + 255) + (2 + 255) + 8 + 8 = 530 bytes; 20 buckets = 10 600,
    // far above the 4084-byte record limit (chunk 11, 11a working).
    const Schema schema({Column{"", "v", Type::Varchar, 255}});
    TableStats stats{20, 20, 255.0, {}};
    ColumnStats v{20, Value::ofVarchar(std::string(255, 'a')), Value::ofVarchar(std::string(255, 'z')), {}};
    for (int i = 0; i < 20; ++i)
    {
        const char c = static_cast<char>('a' + i);
        v.histogram.push_back({Value::ofVarchar(std::string(255, c)), Value::ofVarchar(std::string(254, c) + "~"),
            1, 1});
    }
    stats.columns = {v};

    const std::vector<std::byte> blob = format::encodeStats(stats, schema);
    CHECK(blob.size() == 22 + 8 + 257 + 257 + 2 + 20 * 530);
    const auto records = format::cutIntoChunks("Wide", blob);
    CHECK(records.size() >= 3);
    std::vector<format::StatsChunk> chunks;
    for (const auto& record : records)
    {
        CHECK(record.size() <= format::kMaxCatalogRecord);
        chunks.push_back(format::decodeStatsChunk(record));
    }
    CHECK(format::decodeStats(format::joinChunks(chunks), schema) == stats);
}
