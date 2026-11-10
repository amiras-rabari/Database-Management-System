// Chunk 04: selectivity of a condition, from hand-made column statistics.
//
// Results are compared with a tolerance, not ==. Binary floating point
// cannot hold 0.1 or 0.02 exactly, so 1.0 / 50 * (10.0 / 50) may differ from
// the decimal 0.004 in its last bit; a tolerance of 1e-12 accepts that and
// still catches any real formula mistake.

#include <doctest/doctest.h>

#include <cstdint>

#include <db/common/condition.h>
#include <db/common/schema.h>
#include <db/common/value.h>
#include <db/opt/defaults.h>
#include <db/opt/selectivity.h>
#include <db/storage/table_stats.h>

#include <string>
#include <utility>
#include <vector>

namespace
{
    using db::ColumnRef;
    using db::Condition;
    using db::Value;
    using db::storage::ColumnStats;
    using db::storage::HistogramBucket;

    // Selectivity of `text` with the statistics in `columns`, looked up by the
    // column's full name as written (`Emp.did`). Unknown names have no
    // statistics (nullptr), like a never-analyzed table.
    double sel(const std::string& text, const std::vector<std::pair<std::string, ColumnStats>>& columns)
    {
        db::opt::ColumnStatsLookup lookup = [&columns](const ColumnRef& ref) -> const ColumnStats* {
            for (const auto& [name, stats] : columns)
            {
                if (name == ref.toString())
                {
                    return &stats;
                }
            }
            return nullptr;
        };
        return db::opt::estimateSelectivity(db::parseCondition(text), lookup);
    }

    // Column statistics with ndv/min/max only, as in the §M4.2 example.
    ColumnStats noHistogram(std::uint64_t ndv, std::int32_t min, std::int32_t max)
    {
        return ColumnStats{ndv, Value::ofInt(min), Value::ofInt(max), {}};
    }

    // R.a with values 1..20 in buckets [1,5] [6,10] [11,15] [16,20], each
    // holding 5 rows and 5 distinct values (chunk 02's first test).
    std::vector<std::pair<std::string, ColumnStats>> histogramA()
    {
        ColumnStats a{20, Value::ofInt(1), Value::ofInt(20), {}};
        for (std::int32_t lo : {1, 6, 11, 16})
        {
            a.histogram.push_back(HistogramBucket{Value::ofInt(lo), Value::ofInt(lo + 4), 5, 5});
        }
        return {{"R.a", a}};
    }

    const double kTolerance = 1e-12;
} // namespace

TEST_CASE("§M4.2 worked example: 0.02, 0.20 and 0.004")
{
    // Emp: ndv(did) = 50, age runs 20..70, no histograms.
    std::vector<std::pair<std::string, ColumnStats>> emp = {{"Emp.did", noHistogram(50, 1, 50)},
                                                           {"Emp.age", noHistogram(51, 20, 70)}};

    CHECK(sel("Emp.did = 7", emp) == doctest::Approx(0.02).epsilon(kTolerance));
    CHECK(sel("Emp.age > 60", emp) == doctest::Approx(0.20).epsilon(kTolerance));
    // AND assumes independence: 0.02 x 0.20.
    CHECK(sel("Emp.did = 7 and Emp.age > 60", emp) == doctest::Approx(0.004).epsilon(kTolerance));
}

TEST_CASE("Histogram formulas on 1..20 in four buckets")
{
    auto a = histogramA();
    // count(bucket) / (n x ndv(bucket)) = 5 / (20 x 5).
    CHECK(sel("R.a = 3", a) == doctest::Approx(0.05).epsilon(kTolerance));
    // Bucket [1,5] whole, plus (8 - 6) / (10 - 6) of [6,10]: (5 + 2.5) / 20.
    // The true answer is 7 / 20 = 0.35: interpolation assumes the values are
    // spread evenly over 6..10, and treats them as continuous.
    CHECK(sel("R.a < 8", a) == doctest::Approx(0.375).epsilon(kTolerance));
    // (10 - 8) / 4 of [6,10], plus the two buckets above: (2.5 + 10) / 20.
    CHECK(sel("R.a > 8", a) == doctest::Approx(0.625).epsilon(kTolerance));
    CHECK(sel("R.a <> 3", a) == doctest::Approx(0.95).epsilon(kTolerance));
    // OR: 0.05 + 0.05 - 0.05 x 0.05.
    CHECK(sel("R.a = 3 or R.a = 13", a) == doctest::Approx(0.0975).epsilon(kTolerance));
    CHECK(sel("not (R.a < 8)", a) == doctest::Approx(0.625).epsilon(kTolerance));
    // 8 is the middle of [6,10], where (v - lo) and (hi - v) are equal, so it
    // cannot tell the two fractions apart. 7 can: 1/4 of [6,10] is below it,
    // 3/4 above. a < 7: (5 + 1.25) / 20; a > 7: (3.75 + 10) / 20.
    CHECK(sel("R.a < 7", a) == doctest::Approx(0.3125).epsilon(kTolerance));
    CHECK(sel("R.a > 7", a) == doctest::Approx(0.6875).epsilon(kTolerance));
    // <= is < plus =, >= is > plus = (settled 2026-11-07).
    CHECK(sel("R.a <= 8", a) == doctest::Approx(0.425).epsilon(kTolerance));
    CHECK(sel("R.a >= 8", a) == doctest::Approx(0.675).epsilon(kTolerance));
}

TEST_CASE("A literal on the left is flipped: 8 > a is a < 8")
{
    auto a = histogramA();
    CHECK(sel("8 > R.a", a) == doctest::Approx(sel("R.a < 8", a)).epsilon(kTolerance));
    CHECK(sel("8 <= R.a", a) == doctest::Approx(sel("R.a >= 8", a)).epsilon(kTolerance));
    CHECK(sel("3 = R.a", a) == doctest::Approx(0.05).epsilon(kTolerance));
}

TEST_CASE("Values outside the histogram")
{
    auto a = histogramA();
    CHECK(sel("R.a = 100", a) == 0.0);
    CHECK(sel("R.a < 0", a) == 0.0);
    CHECK(sel("R.a > 100", a) == 0.0);
    CHECK(sel("R.a < 100", a) == doctest::Approx(1.0).epsilon(kTolerance));
}

TEST_CASE("VARCHAR: half a bucket for a range, 0 in a gap")
{
    // Chunk 02's VARCHAR example: ["Ann","Bob"] 3 rows / 2 values, ["Cy","Cy"] 1 row.
    ColumnStats name{3, Value::ofVarchar("Ann"), Value::ofVarchar("Cy"),
                     {{Value::ofVarchar("Ann"), Value::ofVarchar("Bob"), 3, 2},
                      {Value::ofVarchar("Cy"), Value::ofVarchar("Cy"), 1, 1}}};
    std::vector<std::pair<std::string, ColumnStats>> columns = {{"R.name", name}};

    CHECK(sel("R.name = 'Bob'", columns) == doctest::Approx(3.0 / 8.0).epsilon(kTolerance));
    // "Bo" lies inside ["Ann","Bob"]: a string has no "v - lo", so the
    // bucket counts half: 1.5 / 4.
    CHECK(sel("R.name < 'Bo'", columns) == doctest::Approx(0.375).epsilon(kTolerance));
    // "Bz" falls in the gap between "Bob" and "Cy": only ["Cy","Cy"] is above.
    CHECK(sel("R.name > 'Bz'", columns) == doctest::Approx(0.25).epsilon(kTolerance));
    CHECK(sel("R.name = 'Bz'", columns) == 0.0);
}

TEST_CASE("No statistics: the documented defaults")
{
    CHECK(sel("R.a = 3", {}) == db::opt::kDefaultEqualitySelectivity);
    CHECK(sel("R.a < 3", {}) == db::opt::kDefaultRangeSelectivity);
    CHECK(sel("R.a >= 3", {}) == db::opt::kDefaultRangeSelectivity);
    CHECK(sel("R.a <> 3", {}) == db::opt::kDefaultNotEqualSelectivity);
}

TEST_CASE("Two literals: the comparison is simply true or false")
{
    CHECK(sel("1 = 1", {}) == 1.0);
    CHECK(sel("1 = 2", {}) == 0.0);
    CHECK(sel("'b' > 'a'", {}) == 1.0);
}

TEST_CASE("Column against column: the 0.1 default")
{
    auto a = histogramA();
    CHECK(sel("R.a = R.b", a) == db::opt::kDefaultOtherJoinSelectivity);
}

TEST_CASE("A deep AND chain does not overflow the stack")
{
    // 500 nested ANDs of R.a = 3 (0.05 each): 0.05^500 is far below the
    // smallest double, so the product becomes 0.
    auto a = histogramA();
    db::opt::ColumnStatsLookup lookup = [&a](const ColumnRef&) { return &a[0].second; };
    Condition chain = db::parseCondition("R.a = 3");
    for (int i = 1; i < 500; ++i)
    {
        chain = Condition::andOf(chain, db::parseCondition("R.a = 3"));
    }
    CHECK(db::opt::estimateSelectivity(chain, lookup) == 0.0);
}
