#include "db/opt/selectivity.h"

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

#include "db/common/condition.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/opt/defaults.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    namespace
    {
        // A number for interpolation. Only INT columns get here (a table
        // column is INT or VARCHAR, and VARCHAR never interpolates). Turning
        // both ends into double before subtracting avoids int32 overflow:
        // hi - lo near the int32 limits would be undefined behaviour.
        double toNumber(const Value& value)
        {
            return static_cast<double>(value.asInt());
        }

        // The buckets to estimate with. Without a histogram, [min, max] acts
        // as one bucket (count 1 stands for "all rows"), so the range
        // formulas need only one version. This lets the same range math work for both cases
        std::vector<storage::HistogramBucket> bucketsOf(const storage::ColumnStats& stats)
        {
            if (!stats.histogram.empty())
            {
                return stats.histogram;
            }
            return {storage::HistogramBucket{stats.min, stats.max, 1, stats.ndv}};
        }

        // n: every row is in exactly one bucket, so n is their total count.
        double totalCount(const std::vector<storage::HistogramBucket>& buckets)
        {
            double total = 0.0;
            for (const storage::HistogramBucket& bucket : buckets)
            {
                total += static_cast<double>(bucket.count);
            }
            return total;
        }

        // sel(c = v). With a histogram: count(bucket) / (n x ndv(bucket)) for
        // the bucket holding v, f $v$ falls outside all buckets, it returns 0.0 (buckets never overlap,
        // chunk 02, so at most one does). Without histogram: 1 / ndv(c) (§M4.2).
        double equalSelectivity(const storage::ColumnStats& stats, const Value& v)
        {
            if (stats.histogram.empty())
            {
                return 1.0 / static_cast<double>(stats.ndv);
            }
            double n = totalCount(stats.histogram);
            for (const storage::HistogramBucket& bucket : stats.histogram)
            {
                // Searches for the bucket where lo < v < hi
                if (compareValues(bucket.lo, v) <= 0 && compareValues(v, bucket.hi) <= 0)
                {
                    return static_cast<double>(bucket.count) / (n * static_cast<double>(bucket.ndv));
                }
            }
            return 0.0;
        }

        // sel(c < v): whole buckets entirely below v, plus a linear fraction
        // of the bucket with lo < v <= hi: (v - lo) / (hi - lo) of its rows.
        // A VARCHAR bucket counts half instead (settled: a string has no
        // "v - lo"). A one-value bucket (lo = hi) can never have lo < v <= hi.
        double lessSelectivity(const storage::ColumnStats& stats, const Value& v)
        {
            std::vector<storage::HistogramBucket> buckets = bucketsOf(stats);
            double below = 0.0;
            for (const storage::HistogramBucket& bucket : buckets)
            {
                double count = static_cast<double>(bucket.count);
                if (compareValues(bucket.hi, v) < 0)
                {
                    below += count;
                }
                else if (compareValues(bucket.lo, v) < 0)
                {
                    if (v.type() == Type::Varchar)
                    {
                        below += count / 2.0;
                    }
                    else
                    {
                        double lo = toNumber(bucket.lo);
                        double hi = toNumber(bucket.hi);
                        below += count * (toNumber(v) - lo) / (hi - lo);
                    }
                }
            }
            // final step matching rows / N
            return below / totalCount(buckets);
        }

        // sel(c > v): the mirror image. Whole buckets entirely above v, plus
        // (hi - v) / (hi - lo) of the bucket with lo <= v < hi.
        double greaterSelectivity(const storage::ColumnStats& stats, const Value& v)
        {
            std::vector<storage::HistogramBucket> buckets = bucketsOf(stats);
            double above = 0.0;
            for (const storage::HistogramBucket& bucket : buckets)
            {
                double count = static_cast<double>(bucket.count);
                if (compareValues(bucket.lo, v) > 0)
                {
                    above += count;
                }
                else if (compareValues(bucket.hi, v) > 0)
                {
                    if (v.type() == Type::Varchar)
                    {
                        above += count / 2.0;
                    }
                    else
                    {
                        double lo = toNumber(bucket.lo);
                        double hi = toNumber(bucket.hi);
                        above += count * (hi - toNumber(v)) / (hi - lo);
                    }
                }
            }
            // final step matching rows / N
            return above / totalCount(buckets);
        }

        // `literal op column` means the same as `column op' literal` with the
        // comparison turned around: 5 < R.a is R.a > 5. = and <> stay.
        CompareOp flip(CompareOp op)
        {
            switch (op)
            {
            case CompareOp::Less:
                return CompareOp::Greater;
            case CompareOp::LessEqual:
                return CompareOp::GreaterEqual;
            case CompareOp::Greater:
                return CompareOp::Less;
            case CompareOp::GreaterEqual:
                return CompareOp::LessEqual;
            default:
                return op;
            }
        }

        // Whether `op` holds for two values whose compareValues result is
        // `order` (-1, 0 or 1).
        bool holds(CompareOp op, int order)
        {
            switch (op)
            {
            case CompareOp::Equal:
                return order == 0;
            case CompareOp::NotEqual:
                return order != 0;
            case CompareOp::Less:
                return order < 0;
            case CompareOp::LessEqual:
                return order <= 0;
            case CompareOp::Greater:
                return order > 0;
            default:
                return order >= 0;
            }
        }

        double compareSelectivity(const Condition& condition, const ColumnStatsLookup& lookup)
        {
            const ColumnRef* leftColumn = std::get_if<ColumnRef>(&condition.left);
            const ColumnRef* rightColumn = std::get_if<ColumnRef>(&condition.right);

            // Two literals (`1 = 1`): the comparison is simply true or false.
            if (!leftColumn && !rightColumn)
            {
                int order = compareValues(std::get<Value>(condition.left), std::get<Value>(condition.right));
                return holds(condition.op, order) ? 1.0 : 0.0;
            }

            // Two columns inside one select (`R.a = R.b`): no formula in
            // §M4.2 fits, so the "any other join predicate" default, 0.1.
            if (leftColumn && rightColumn)
            {
                return kDefaultOtherJoinSelectivity;
            }

            // One column and one literal: write it as `column op literal`.
            const ColumnRef& column = leftColumn ? *leftColumn : *rightColumn;
            const Value& v = leftColumn ? std::get<Value>(condition.right) : std::get<Value>(condition.left);
            CompareOp op = leftColumn ? condition.op : flip(condition.op);

            // If no stats exist, returns fixed defaults
            const storage::ColumnStats* stats = lookup(column);
            if (stats == nullptr)
            {
                // Nothing known about the column: the documented defaults.
                if (op == CompareOp::Equal)
                {
                    return kDefaultEqualitySelectivity;
                }
                if (op == CompareOp::NotEqual)
                {
                    return kDefaultNotEqualSelectivity;
                }
                return kDefaultRangeSelectivity;
            }

            switch (op)
            {
            case CompareOp::Equal:
                return equalSelectivity(*stats, v);
            case CompareOp::NotEqual:
                return 1.0 - equalSelectivity(*stats, v);
            case CompareOp::Less:
                return lessSelectivity(*stats, v);
            case CompareOp::LessEqual:
                return std::min(1.0, lessSelectivity(*stats, v) + equalSelectivity(*stats, v));
            case CompareOp::Greater:
                return greaterSelectivity(*stats, v);
            default: // GreaterEqual
                return std::min(1.0, greaterSelectivity(*stats, v) + equalSelectivity(*stats, v));
            }
        }
    } // namespace

    double estimateSelectivity(const Condition& condition, const ColumnStatsLookup& lookup)
    {
        // Recursive over the tree. Trees are at most kMaxNestingDepth (1000)
        // deep and each call keeps only a few numbers, so the stack is safe.
        switch (condition.kind)
        {
        case ConditionKind::Compare:
            return compareSelectivity(condition, lookup);

        case ConditionKind::And:
            {
                // p AND q = sel(p) x sel(q): assumes p and q are independent.
                // Children are taken in their written order, so the floating
                // point result is the same on every run.
                double result = 1.0;
                for (const Condition& child : condition.children)
                {
                    result *= estimateSelectivity(child, lookup);
                }
                return result;
            }

        case ConditionKind::Or:
            {
                // p OR q = sel(p) + sel(q) - sel(p) x sel(q), applied pairwise in
                // written order. Starting from 0 gives the first child unchanged.
                // applies inclusion-exclusion across children
                double result = 0.0;
                for (const Condition& child : condition.children)
                {
                    double s = estimateSelectivity(child, lookup);
                    result = result + s - result * s;
                }
                return result;
            }

        default: // Not
            return 1.0 - estimateSelectivity(condition.children[0], lookup);
        }
    }
} // namespace db::opt
