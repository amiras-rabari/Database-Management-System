// Chunk 02: building one column's statistics from its sorted values.
// All data is our own (the instructor will not post an M4 workload).

#include <doctest/doctest.h>

#include <db/common/value.h>
#include <db/opt/histogram.h>
#include <db/storage/table_stats.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    using db::Type;
    using db::Value;
    using db::storage::ColumnStats;
    using db::storage::HistogramBucket;

    // Feeds `values` (already sorted) to buildColumnStats, the way ANALYZE
    // feeds the external sort's output.
    ColumnStats build(Type type, const std::vector<Value>& values, std::int32_t buckets)
    {
        std::size_t next = 0;
        return db::opt::buildColumnStats(type, values.size(), buckets, [&](Value& out) {
            if (next == values.size())
            {
                return false;
            }
            out = values[next];
            ++next;
            return true;
        });
    }

    std::vector<Value> ints(const std::vector<std::int32_t>& numbers)
    {
        std::vector<Value> values;
        for (std::int32_t number : numbers)
        {
            values.push_back(Value::ofInt(number));
        }
        return values;
    }

    HistogramBucket intBucket(std::int32_t lo, std::int32_t hi, std::uint64_t count, std::uint64_t ndv)
    {
        return HistogramBucket{Value::ofInt(lo), Value::ofInt(hi), count, ndv};
    }
} // namespace

TEST_CASE("Uniform values, exact cut: 1..20 into 4 buckets of 5")
{
    std::vector<std::int32_t> numbers;
    for (std::int32_t v = 1; v <= 20; ++v)
    {
        numbers.push_back(v);
    }
    ColumnStats stats = build(Type::Int, ints(numbers), 4);

    CHECK(stats.ndv == 20);
    CHECK(stats.min == Value::ofInt(1));
    CHECK(stats.max == Value::ofInt(20));
    CHECK(stats.histogram == std::vector<HistogramBucket>{intBucket(1, 5, 5, 5), intBucket(6, 10, 5, 5),
                                                         intBucket(11, 15, 5, 5), intBucket(16, 20, 5, 5)});
}

TEST_CASE("Runs aligned with cuts: 0..99 ten times each into 20 buckets")
{
    std::vector<std::int32_t> numbers;
    for (std::int32_t v = 0; v < 100; ++v)
    {
        for (int copy = 0; copy < 10; ++copy)
        {
            numbers.push_back(v);
        }
    }
    ColumnStats stats = build(Type::Int, ints(numbers), 20);

    CHECK(stats.ndv == 100);
    std::vector<HistogramBucket> expected;
    for (std::int32_t i = 0; i < 20; ++i)
    {
        expected.push_back(intBucket(5 * i, 5 * i + 4, 50, 5));
    }
    CHECK(stats.histogram == expected);
}

TEST_CASE("Skew: a run of equal values is never split at a cut")
{
    // n = 10, k = 2: the cut is after position 5, inside the run of six 1s.
    // The bucket stays open until the value changes.
    ColumnStats stats = build(Type::Int, ints({1, 1, 1, 1, 1, 1, 2, 3, 4, 5}), 2);

    CHECK(stats.ndv == 5);
    CHECK(stats.histogram == std::vector<HistogramBucket>{intBucket(1, 1, 6, 1), intBucket(2, 5, 4, 4)});

    // k = 5: cuts after positions 2, 4, 6, 8, 10. The six 1s pass the cuts
    // at 2, 4 and 6, so the next bucket ends at 8 (not at 4 or 6), and the
    // histogram has 3 buckets instead of 5.
    ColumnStats wide = build(Type::Int, ints({1, 1, 1, 1, 1, 1, 2, 3, 4, 5}), 5);
    CHECK(wide.histogram ==
          std::vector<HistogramBucket>{intBucket(1, 1, 6, 1), intBucket(2, 3, 2, 2), intBucket(4, 5, 2, 2)});
}

TEST_CASE("Fewer values than buckets: no empty bucket")
{
    ColumnStats stats = build(Type::Int, ints({7, 8}), 20);

    CHECK(stats.ndv == 2);
    CHECK(stats.histogram == std::vector<HistogramBucket>{intBucket(7, 7, 1, 1), intBucket(8, 8, 1, 1)});
}

TEST_CASE("One distinct value: a single bucket")
{
    ColumnStats stats = build(Type::Int, ints(std::vector<std::int32_t>(1000, 42)), 20);

    CHECK(stats.ndv == 1);
    CHECK(stats.min == Value::ofInt(42));
    CHECK(stats.max == Value::ofInt(42));
    CHECK(stats.histogram == std::vector<HistogramBucket>{intBucket(42, 42, 1000, 1)});
}

TEST_CASE("VARCHAR: byte-wise order, string bounds")
{
    // n = 4, k = 2: the cut after position 2 falls inside the run of "Bob",
    // so the first bucket takes both copies.
    std::vector<Value> values = {Value::ofVarchar("Ann"), Value::ofVarchar("Bob"), Value::ofVarchar("Bob"),
                                 Value::ofVarchar("Cy")};
    ColumnStats stats = build(Type::Varchar, values, 2);

    CHECK(stats.ndv == 3);
    CHECK(stats.min == Value::ofVarchar("Ann"));
    CHECK(stats.max == Value::ofVarchar("Cy"));
    CHECK(stats.histogram ==
          std::vector<HistogramBucket>{{Value::ofVarchar("Ann"), Value::ofVarchar("Bob"), 3, 2},
                                       {Value::ofVarchar("Cy"), Value::ofVarchar("Cy"), 1, 1}});

    // Byte-wise: "Z" (0x5A) sorts before "a" (0x61), so a sorted stream has
    // "Z" first and it becomes the minimum.
    ColumnStats mixed = build(Type::Varchar, {Value::ofVarchar("Z"), Value::ofVarchar("a")}, 1);
    CHECK(mixed.min == Value::ofVarchar("Z"));
    CHECK(mixed.max == Value::ofVarchar("a"));
    CHECK(mixed.histogram.size() == 1);
}

TEST_CASE("Determinism: the same stream gives identical statistics")
{
    std::vector<Value> values = ints({1, 1, 2, 3, 3, 3, 4, 7, 7, 9, 10, 10, 12});
    CHECK(build(Type::Int, values, 4) == build(Type::Int, values, 4));
}

TEST_CASE("Empty column: ndv 0, no buckets, typed placeholders")
{
    ColumnStats intStats = build(Type::Int, {}, 20);
    CHECK(intStats.ndv == 0);
    CHECK(intStats.histogram.empty());
    CHECK(intStats.min == Value::ofInt(0));
    CHECK(intStats.max == Value::ofInt(0));

    // Storage refuses a min/max of the wrong type, so a VARCHAR column gets
    // empty strings, not INT 0.
    ColumnStats textStats = build(Type::Varchar, {}, 20);
    CHECK(textStats.ndv == 0);
    CHECK(textStats.histogram.empty());
    CHECK(textStats.min == Value::ofVarchar(""));
    CHECK(textStats.max == Value::ofVarchar(""));
}
