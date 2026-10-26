#pragma once

/// @file
/// The buffer pool's five counters (§M1.6 "stats() -> { hits, misses,
/// evictions, reads, writes }"), plus adding and subtracting them.
///
/// How to measure the I/O of one piece of work: take a snapshot before
/// (`before = pool.stats()`), do the work, then compute
/// `pool.stats() - before`. That difference is exactly what the work cost.
/// The pool's counters only ever go up during a run, so the difference is
/// never negative, and the same run always gives the same difference.

#include <cassert>
#include <cstdint>

namespace db::storage
{

    /// The five counters. A plain struct: `IoStats{}` is all zeros, and
    /// copying it takes a snapshot that later pool activity does not change.
    /// What each counter counts (§M1.6, `DESIGN_M1.md`):
    /// - hits: fetches that found the page already in memory;
    /// - misses: fetches that had to read the page from disk;
    /// - evictions: misses that threw out another page to make room
    ///   (filling an empty frame is not one; newPage never counts one);
    /// - reads / writes: calls to the disk manager's readPage / writePage.
    struct IoStats
    {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t reads = 0;
        std::uint64_t writes = 0;

        bool operator==(const IoStats&) const = default;
    };

    /// Adds two sets of counters field by field, e.g. the I/O of several operators.
    inline IoStats operator+(const IoStats& a, const IoStats& b)
    {
        return IoStats{a.hits + b.hits, a.misses + b.misses, a.evictions + b.evictions,
                       a.reads + b.reads, a.writes + b.writes};
    }

    /// `after - before`, field by field: the I/O done between two snapshots.
    /// `after` must be the later snapshot. The counters are unsigned, so
    /// subtracting the wrong way round would not give a negative number but
    /// a huge one (e.g. 3 - 5 = 18446744073709551614). Debug builds stop
    /// with an assertion if that happens.
    inline IoStats operator-(const IoStats& after, const IoStats& before)
    {
        assert(after.hits >= before.hits && after.misses >= before.misses
               && after.evictions >= before.evictions && after.reads >= before.reads
               && after.writes >= before.writes && "IoStats: after - before with before later");
        return IoStats{after.hits - before.hits, after.misses - before.misses,
                       after.evictions - before.evictions, after.reads - before.reads,
                       after.writes - before.writes};
    }

} // namespace db::storage
