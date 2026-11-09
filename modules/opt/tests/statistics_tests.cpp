// Chunk 01: reading one table's statistics, with and without ANALYZE.
// Every table here is a TableInfo built in memory, the way a later test fakes
// the spec's made-up tables without any disk file.

#include <doctest/doctest.h>

#include <db/common/error.h>
#include <db/common/schema.h>
#include <db/common/value.h>
#include <db/opt/defaults.h>
#include <db/opt/statistics.h>
#include <db/storage/table_info.h>
#include <db/storage/table_stats.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using db::Column;
    using db::ColumnRef;
    using db::Schema;
    using db::Type;
    using db::Value;
    using db::storage::ColumnStats;
    using db::storage::TableInfo;
    using db::storage::TableStats;

    // Emp(eid INT, name VARCHAR(20), did INT, age INT, salary INT), qualified
    // the way Catalog::getTable returns it.
    Schema empSchema()
    {
        return Schema({{"Emp", "eid", Type::Int, 0},
                       {"Emp", "name", Type::Varchar, 20},
                       {"Emp", "did", Type::Int, 0},
                       {"Emp", "age", Type::Int, 0},
                       {"Emp", "salary", Type::Int, 0}});
    }

    // Statistics for one INT column without a histogram.
    ColumnStats intColumn(std::uint64_t ndv, std::int32_t min, std::int32_t max)
    {
        ColumnStats column;
        column.ndv = ndv;
        column.min = Value::ofInt(min);
        column.max = Value::ofInt(max);
        return column;
    }

    // A table with catalog counts and, optionally, ANALYZE's statistics.
    TableInfo makeTable(const std::string& name, Schema schema, std::uint32_t pages,
                        std::uint64_t rows, std::optional<TableStats> stats)
    {
        TableInfo info{name, std::move(schema), name + ".db", 0, pages, rows, std::move(stats)};
        return info;
    }

    // Emp from §M4.2: 10,000 tuples on 200 pages, ndv(did) = 50, age 20..70.
    TableInfo analyzedEmp(double width)
    {
        TableStats stats;
        stats.nTuples = 10000;
        stats.nPages = 200;
        stats.width = width;
        ColumnStats name;
        name.ndv = 10000;
        name.min = Value::ofVarchar("Aaron");
        name.max = Value::ofVarchar("Zoe");
        stats.columns = {intColumn(10000, 1, 10000), name, intColumn(50, 1, 50),
                         intColumn(51, 20, 70), intColumn(5000, 30000, 90000)};
        return makeTable("Emp", empSchema(), 200, 10000, stats);
    }
} // namespace

TEST_CASE("Emp from §M4.2: column statistics by qualified and bare name")
{
    TableInfo emp = analyzedEmp(30.0);

    const ColumnStats* did = db::opt::columnStats(emp, ColumnRef{"Emp", "did"});
    REQUIRE(did != nullptr);
    CHECK(did->ndv == 50);

    // A bare name resolves when exactly one column has it.
    const ColumnStats* age = db::opt::columnStats(emp, ColumnRef{std::nullopt, "age"});
    REQUIRE(age != nullptr);
    CHECK(age->min == Value::ofInt(20));
    CHECK(age->max == Value::ofInt(70));

    // An analyzed table reports ANALYZE's width.
    CHECK(db::opt::tableWidth(emp) == 30.0);
}

TEST_CASE("Dept from §M4.2: ndv(did) = 50")
{
    Schema schema({{"Dept", "did", Type::Int, 0}, {"Dept", "budget", Type::Int, 0}});
    TableStats stats;
    stats.nTuples = 50;
    stats.nPages = 1;
    stats.width = 8.0;
    stats.columns = {intColumn(50, 1, 50), intColumn(40, 100, 5000)};
    TableInfo dept = makeTable("Dept", schema, 1, 50, stats);

    const ColumnStats* did = db::opt::columnStats(dept, ColumnRef{"Dept", "did"});
    REQUIRE(did != nullptr);
    CHECK(did->ndv == 50);
}

TEST_CASE("No statistics: width from the catalog counts, no column statistics")
{
    // 1,000 rows on 10 pages, never analyzed: width = 10 * 4096 / 1000.
    TableInfo emp = makeTable("Emp", empSchema(), 10, 1000, std::nullopt);

    CHECK(db::opt::tableWidth(emp) == doctest::Approx(40.96));
    CHECK(db::opt::columnStats(emp, ColumnRef{"Emp", "did"}) == nullptr);

    // The documented defaults the estimators fall back to (decision 1 (a));
    // 0.1 for other join predicates and 20 buckets are fixed by §M4.2/§M4.1.
    CHECK(db::opt::kDefaultEqualitySelectivity == 0.1);
    CHECK(db::opt::kDefaultRangeSelectivity == doctest::Approx(1.0 / 3.0));
    CHECK(db::opt::kDefaultNotEqualSelectivity == 0.9);
    CHECK(db::opt::kDefaultOtherJoinSelectivity == 0.1);
    CHECK(db::opt::kDefaultBuckets == 20);
}

TEST_CASE("Empty table: width from the schema, and ndv 0 means nothing known")
{
    // Never analyzed and 0 rows: INT 4 bytes each, VARCHAR(20) 2 + 20.
    TableInfo empty = makeTable("Emp", empSchema(), 0, 0, std::nullopt);
    CHECK(db::opt::tableWidth(empty) == 4.0 + 22.0 + 4.0 + 4.0 + 4.0);

    // Analyzed while empty: every column has ndv 0, so no statistics to use.
    TableStats stats;
    stats.columns = {intColumn(0, 0, 0), intColumn(0, 0, 0), intColumn(0, 0, 0),
                     intColumn(0, 0, 0), intColumn(0, 0, 0)};
    stats.columns[1].min = Value::ofVarchar("");
    stats.columns[1].max = Value::ofVarchar("");
    TableInfo analyzedEmpty = makeTable("Emp", empSchema(), 0, 0, stats);
    CHECK(db::opt::columnStats(analyzedEmpty, ColumnRef{"Emp", "did"}) == nullptr);
}

TEST_CASE("Unknown column is a name error, with or without statistics")
{
    TableInfo analyzed = analyzedEmp(30.0);
    TableInfo plain = makeTable("Emp", empSchema(), 10, 1000, std::nullopt);

    for (const TableInfo* info : {&analyzed, &plain})
    {
        try
        {
            db::opt::columnStats(*info, ColumnRef{"Emp", "bonus"});
            FAIL("expected Error(Name)");
        }
        catch (const db::Error& error)
        {
            CHECK(error.category() == db::ErrorCategory::Name);
        }
    }
}

TEST_CASE("Column widths: INT 4 bytes, VARCHAR columns share the rest")
{
    // Emp with width 30: four INT columns take 16, so name gets 30 - 16 = 14.
    TableInfo emp = analyzedEmp(30.0);
    std::vector<double> widths = db::opt::columnWidths(emp);
    CHECK(widths == std::vector<double>{4.0, 14.0, 4.0, 4.0, 4.0});

    // Two VARCHAR columns share the rest equally: 20 - 4 = 16, 8 each.
    Schema schema({{"T", "a", Type::Int, 0},
                   {"T", "b", Type::Varchar, 10},
                   {"T", "c", Type::Varchar, 30}});
    TableStats stats;
    stats.width = 20.0;
    stats.columns = {intColumn(1, 1, 1), ColumnStats{}, ColumnStats{}};
    TableInfo t = makeTable("T", schema, 1, 1, stats);
    CHECK(db::opt::columnWidths(t) == std::vector<double>{4.0, 8.0, 8.0});

    // Only INT columns: 4 bytes each.
    Schema ints({{"U", "a", Type::Int, 0}, {"U", "b", Type::Int, 0}});
    TableInfo u = makeTable("U", ints, 1, 100, std::nullopt);
    CHECK(db::opt::columnWidths(u) == std::vector<double>{4.0, 4.0});
}
