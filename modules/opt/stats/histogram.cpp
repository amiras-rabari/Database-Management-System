#include "db/opt/histogram.h"

#include <cstdint>
#include <functional>

#include "db/common/value.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    namespace
    {
        // The position after which bucket `index` (from 0) should end:
        // (index + 1) * n / k, in whole numbers. Floating point could round
        // differently on another machine and move a boundary. With n = 10,
        // k = 2: bucket 0 ends after position 5, bucket 1 after 10.
        std::uint64_t cutPosition(std::uint64_t index, std::uint64_t n, std::uint64_t buckets)
        {
            return (index + 1) * n / buckets;
        }
    } // namespace

    storage::ColumnStats buildColumnStats(Type type, std::uint64_t n, std::int32_t buckets,
                                          const std::function<bool(Value&)>& next)
    {
        storage::ColumnStats result;
        const std::uint64_t k = static_cast<std::uint64_t>(buckets);

        // Empty column: nothing to count. min and max still need the column's
        // type, so use 0 or the empty string as placeholders.
        if (n == 0)
        {
            result.min = type == Type::Varchar ? Value::ofVarchar("") : Value::ofInt(0);
            result.max = result.min;
            return result;
        }

        // The current bucket is number `bucketIndex`; it may close once
        // `position` (values taken so far) has reached `cut`.
        std::uint64_t bucketIndex = 0;
        std::uint64_t cut = cutPosition(bucketIndex, n, k);
        std::uint64_t position = 0;

        storage::HistogramBucket bucket;
        Value value = Value::ofInt(0);
        Value previous = Value::ofInt(0);
        bool first = true;

        while (next(value))
        {
            // The input is sorted, so a value is new exactly when it differs
            // from the one before. This counts distinct values without
            // keeping a set of them.
            bool isNewValue = first || compareValues(value, previous) != 0;

            if (first)
            {
                // The very first value opens bucket 0 and is the minimum.
                result.min = value;
                bucket = storage::HistogramBucket{value, value, 0, 0};
            }
            else if (isNewValue && position >= cut)
            {
                // The bucket is full. It closes only between two different
                // values, so a run of equal values that crosses the cut
                // stayed in it (six 1s with the cut after 5: count 6).
                result.histogram.push_back(bucket);

                // A long run may have passed several cut points; the next
                // bucket ends at the first cut point still ahead. The last
                // cut point is n, and a value is still coming, so this stops.
                while (cut <= position)
                {
                    ++bucketIndex;
                    cut = cutPosition(bucketIndex, n, k);
                }
                bucket = storage::HistogramBucket{value, value, 0, 0};
            }

            // Add the value to the current bucket. A new bucket always starts
            // with a new value, so the bucket's ndv and the column's ndv grow
            // together.
            bucket.hi = value;
            ++bucket.count;
            if (isNewValue)
            {
                ++bucket.ndv;
                ++result.ndv;
            }

            previous = value;
            first = false;
            ++position;
        }

        // The last bucket is still open; the last value is the maximum.
        result.histogram.push_back(bucket);
        result.max = previous;
        return result;
    }
} // namespace db::opt
