#pragma once

/// @file
/// The I/O cost model (§M4.4): cost = page reads + page writes, CPU ignored.
/// M and N are the estimated page counts of the inputs (chunk 05), B the
/// frames the operator may use. Every formula counts one read of each input,
/// as §M4.4 does (a one-pass sort costs M).
///
/// Operators with no I/O of their own cost 0 and need no function: filter,
/// project, rename, limit and union_all are pipelined (§M4.4). seqscan costs
/// its table's M data pages (§M4.1: directory pages not counted).
///
/// The set operations are built from costSort, the way exec runs them
/// (DECISIONS 2026-11-07): union = costSort(L + R, B) (one sort of both
/// inputs); intersect and except = costSort(M, B) + costSort(N, B) (both
/// sorts stream their last pass into the merge). Sort-mode distinct and
/// group_by = costSort(M, B).
///
/// Page counts are whole numbers and every ceil is integer division, never
/// floating point. B >= 3 (the CLI checks `--frames`).

#include <cstdint>

namespace db::opt
{
    /// §M4.4 sort: 2 x M x (passes - 1) + M, with passes =
    /// 1 + ceil(log(ceil(M/B)) / log(B-1)) (source conflict #6) taken from
    /// exec's expectedSortPasses, so opt and exec count passes the same way.
    /// Equals exec's measured sort I/O (rewrite variant, DECISIONS 2026-11-06):
    /// costSort(8, 3) = 40.
    std::int64_t costSort(std::int64_t m, std::int64_t b);

    /// §M4.4 block nested loop: M + ceil(M / (B - 2)) x N. M is the outer
    /// (read once, B - 2 pages at a time), N the inner (read once per block).
    std::int64_t costNestedLoopJoin(std::int64_t m, std::int64_t n, std::int64_t b);

    /// Sort-merge join: costSort(M) + costSort(N). §M4.4 adds "+ M + N" for
    /// the merge's reads; exec's sorts stream their last pass straight into
    /// the merge, so those reads are already inside the sorts and exec never
    /// does them twice (decided 2026-11-09, option (b); §M4.4: "say which is
    /// right and fix the other").
    std::int64_t costSortMergeJoin(std::int64_t m, std::int64_t n, std::int64_t b);

    /// §M4.4 grace hash join: 3 x (M + N): read both inputs, write them as
    /// partitions, read the partitions back. Exec measures a little more
    /// (partly full last pages, overflow; REPORT_M2.md); the report explains it.
    std::int64_t costHashJoin(std::int64_t m, std::int64_t n);

    /// Hash-mode distinct or group_by (§M4.4 "the hash partitioning it
    /// uses"), costed the way exec runs it (DECISIONS 2026-11-07, Hashing):
    /// - the groups fit in the table (groupPages <= tableFrames): read the
    ///   input once, M;
    /// - else the full table is written to a spill run and read back
    ///   (2 x tableFrames), and the input is written to partitions and read
    ///   back (2 x M): 3 x M + 2 x tableFrames.
    /// `groupPages` = the estimated pages of the output (one row per group);
    /// `tableFrames` = the frames exec gives the table (divideFrames).
    /// Not counted: a partition with too many groups is sorted (it depends
    /// on how the data hashes); the report explains that gap.
    std::int64_t costHashGroup(std::int64_t m, std::int64_t groupPages, std::int64_t tableFrames);
} // namespace db::opt
