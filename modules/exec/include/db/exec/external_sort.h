#pragma once

/// @file
/// External merge sort (SPEC §M2.3, chunk 07). It sorts an input of any
/// size inside a fixed number of frames: tuples that do not fit are written
/// to temporary run files and merged back.
///
/// The sort operator uses it, and so do the sort-based operators of later
/// chunks and M4's ANALYZE (DEPENDENCIES E3), which can call it directly
/// without building a plan.
///
/// How it works, for N input pages:
/// - Pass 0 (add): tuples fill a buffer of `runFrames` pages. When the next
///   tuple does not fit, the buffer is sorted with std::sort (allowed: it is
///   one run that fits in memory, §0.2) and written as one run. So there
///   are ceil(N / runFrames) runs of `runFrames` pages, the last one shorter.
/// - Merge passes (finishInput): merge (mergeFrames - 1) runs at a time
///   into one new run, 1 frame for the output page. A leftover group of one
///   run is copied too: the **rewrite** variant (D6, DECISIONS 2026-11-06),
///   so the I/O equals M4's 2*M*(passes-1) + M. Repeat until at most
///   (finalFrames - 1) runs are left.
/// - Final pass (next): merges those runs while the caller pulls tuples. It
///   writes nothing.
/// - If the whole input fits in the buffer, and in finalFrames, nothing is
///   written: one run, one pass, streamed straight from memory.
///
/// Counting: every run page is written once and read once per pass (chunk
/// 06 run files). One comparison = one call of the tuple comparator, however
/// many key columns it looks at (Q2 (a)). Same input, keys and frames give
/// the same counts on every run.
///
/// Example (the spec's, N = 8 pages, all frames 3): runs of 3, 3, 2 pages
/// (8 writes); pass 1 merges the first two and copies the third (8 reads,
/// 8 writes); pass 2 streams the last 2 runs (8 reads). 3 passes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/exec/tuple_buffer.h"
#include "db/storage/storage_engine.h"
#include "db/storage/temp_file.h"

namespace db::exec
{
    /// Reads one run file (private to exec, sort/run_file.h). Only named
    /// here, so code that uses the sorter does not need exec's private headers.
    class RunReader;

    /// What a sort did, for tests, the trace and the report (§M2.6
    /// measurement 2). Page reads and writes are not here: they are counted
    /// by the pool, and whoever calls the sorter takes the difference
    /// (an Operator does that by itself).
    struct SortStats
    {
        /// Pass 0 plus every merge pass, including the streaming one.
        std::size_t passes = 0;
        /// Runs made by pass 0 (0 for an empty input).
        std::size_t initialRuns = 0;
    };

    /// The pass formula of §M2.3: 1 + ceil(log(ceil(N/B)) / log(B-1)),
    /// for N pages and B frames (B >= 3). Worked out with whole numbers:
    /// runs = ceil(N/B), then runs = ceil(runs / (B-1)) until it is 1, plus
    /// one pass for each step. Floating-point logs can round the wrong way
    /// (log(27)/log(3) may come out just above 3), whole numbers cannot.
    std::size_t expectedSortPasses(std::size_t pages, std::size_t frames);

    /// One external sort. Use it in this order: add every input tuple,
    /// finishInput() once, then next() until it returns false. Destroying
    /// the sorter deletes every run it still has and gives back its frames.
    ///
    /// Frames it holds, at most: runFrames during add (the buffer; the page
    /// a run is written to is one of them, Q3 (a)); mergeFrames during
    /// finishInput; finalFrames during next. Each must be at least 2, and
    /// mergeFrames at least 3 (divideFrames guarantees both).
    ///
    /// Not copyable or movable: its readers refer to its own runs.
    class ExternalSorter
    {
    public:
        /// `keys` are resolved against `schema` here: an unknown column ->
        /// db::Error(Name), ambiguous -> Error(Ambiguity). No I/O, no frame
        /// taken yet. Every comparison adds 1 to `comparisons`. `engine`,
        /// `schema` and `comparisons` must outlive the sorter.
        ExternalSorter(storage::StorageEngine& engine, const Schema& schema, const std::vector<SortKey>& keys,
                       std::size_t runFrames, std::size_t mergeFrames, std::size_t finalFrames,
                       std::uint64_t& comparisons);
        /// Written in the .cpp, where RunReader is a complete type.
        ~ExternalSorter();

        ExternalSorter(const ExternalSorter&) = delete;
        ExternalSorter& operator=(const ExternalSorter&) = delete;

        /// Pass 0: keeps `tuple`, and writes a run first when the buffer is
        /// full. A tuple too long for a page -> db::Error(Schema) (run files).
        void add(const Tuple& tuple);

        /// The input has ended: writes the last run, does every merge pass
        /// but the last, and gets the final merge ready.
        void finishInput();

        /// Fills `out` with the next tuple in sorted order and returns true,
        /// or returns false at the end.
        bool next(Tuple& out);

        const SortStats& stats() const { return stats_; }

    private:
        /// Compares two tuples on the sort keys: negative if `a` comes
        /// first, 0 if equal on every key, positive if `b` comes first.
        /// Counts one comparison.
        int compareTuples(const Tuple& a, const Tuple& b);
        /// Sorts the buffer largest first, so popBack takes the smallest.
        void sortBufferLargestFirst();
        /// Sorts the buffer and writes it as a new run, emptying the buffer.
        void writeBufferAsRun();
        /// One merge pass: groups of `fanIn` runs become one run each.
        void mergePass(std::size_t fanIn);
        /// Opens one reader on each of runs_[first, last) and reads its first tuple.
        void startMerge(std::size_t first, std::size_t last);
        /// The smallest head tuple among the readers of startMerge.
        bool nextMerged(Tuple& out);

        storage::StorageEngine& engine_;
        const Schema& schema_;
        /// The keys, and where each key's column is in a tuple.
        std::vector<SortKey> keys_;
        std::vector<std::size_t> keyColumns_;
        std::size_t mergeFrames_;
        std::size_t finalFrames_;
        std::uint64_t& comparisons_;
        SortStats stats_;

        /// Pass 0's memory, counted against the frames.
        BoundedTupleBuffer buffer_;
        /// True when the whole input stayed in buffer_ (nothing written).
        bool inMemory_ = false;
        /// The runs on disk, in the order they were made. Declared before
        /// the readers on purpose: members are destroyed in reverse order,
        /// so the readers unpin their pages before the runs are deleted.
        std::vector<storage::TempFile> runs_;
        /// One reader per run being merged, its current (head) tuple, and
        /// whether it still has one. One tuple per run: the pages are the
        /// readers' pinned frames.
        std::vector<std::unique_ptr<RunReader>> readers_;
        std::vector<Tuple> heads_;
        std::vector<bool> hasHead_;
    };
} // namespace db::exec
