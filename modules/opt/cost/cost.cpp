#include "db/opt/cost.h"

#include <cstddef>
#include <cstdint>

#include "db/exec/external_sort.h"

namespace db::opt
{
    namespace
    {
        /// ceil(a / b) for positive whole numbers, without floating point.
        std::int64_t ceilDivide(std::int64_t a, std::int64_t b)
        {
            return (a + b - 1) / b;
        }
    } // namespace

    std::int64_t costSort(std::int64_t m, std::int64_t b)
    {
        auto passes = static_cast<std::int64_t>(
            exec::expectedSortPasses(static_cast<std::size_t>(m), static_cast<std::size_t>(b)));
        // Every pass but the last reads and writes all M pages; the last one
        // only reads them (its output streams to the parent).
        return 2 * m * (passes - 1) + m;
    }

    std::int64_t costNestedLoopJoin(std::int64_t m, std::int64_t n, std::int64_t b)
    {
        // The outer is read once, in blocks of B - 2 pages (§M4.4 keeps two
        // frames back, the textbook's inner-input and output pages); the
        // inner is read once per block.
        return m + ceilDivide(m, b - 2) * n;
    }

    std::int64_t costSortMergeJoin(std::int64_t m, std::int64_t n, std::int64_t b)
    {
        return costSort(m, b) + costSort(n, b);
    }

    std::int64_t costHashJoin(std::int64_t m, std::int64_t n)
    {
        return 3 * (m + n);
    }

    std::int64_t costHashGroup(std::int64_t m, std::int64_t groupPages, std::int64_t tableFrames)
    {
        if (groupPages <= tableFrames)
        {
            return m;
        }
        return 3 * m + 2 * tableFrames;
    }
} // namespace db::opt
