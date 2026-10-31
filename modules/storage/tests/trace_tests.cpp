// runTrace (chunk 15): the §M1.6 worked trace for both policies, determinism,
// empty trace, sequential flooding under LRU, reads == misses and
// writes == 0 for every trace, the scratch file always deleted, argument errors.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <doctest/doctest.h>

#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/trace.h"

namespace
{
    namespace fs = std::filesystem;
    using db::storage::IoStats;
    using db::storage::kInvalidPageId;
    using db::storage::PageId;
    using db::storage::ReplacementPolicy;
    using db::storage::runTrace;

    // A fresh folder under the system temp directory, removed when the test
    // starts and again when it ends. Each test case uses its own name.
    class TempDir
    {
    public:
        explicit TempDir(const std::string& name)
            : path_(fs::temp_directory_path() / "comp3005_storage_tests" / name)
        {
            fs::remove_all(path_);
            fs::create_directories(path_);
        }
        ~TempDir()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
        const fs::path& path() const { return path_; }

    private:
        fs::path path_;
    };

    fs::path scratchPath(const fs::path& dir)
    {
        return dir / "tmp" / "trace_scratch.db";
    }

    const std::vector<PageId> kSpecTrace{1, 2, 3, 1, 4, 2};

    // Pages 0..pages-1 in order, repeated `times` times.
    std::vector<PageId> sequentialScans(PageId pages, int times)
    {
        std::vector<PageId> ids;
        for (int t = 0; t < times; ++t)
            for (PageId p = 0; p < pages; ++p)
                ids.push_back(p);
        return ids;
    }

    // `count` ids in 0..range-1 from a fixed linear congruential generator
    // (same seed, same list on every run and platform).
    std::vector<PageId> seededTrace(std::size_t count, PageId range, std::uint32_t seed)
    {
        std::vector<PageId> ids;
        std::uint32_t state = seed;
        for (std::size_t i = 0; i < count; ++i)
        {
            state = state * 1664525u + 1013904223u;
            ids.push_back((state >> 8) % range);
        }
        return ids;
    }

    // The invariants of every trace: each fetch is a hit or a miss, each
    // miss is one read, clean pages are never written.
    void checkInvariants(const IoStats& stats, std::size_t fetches)
    {
        CHECK(stats.hits + stats.misses == fetches);
        CHECK(stats.reads == stats.misses);
        CHECK(stats.writes == 0);
        CHECK(stats.evictions <= stats.misses);
    }
} // namespace

TEST_CASE("runTrace reproduces the SPEC M1.6 trace 1 2 3 1 4 2 at B = 3")
{
    TempDir dir("trace_spec");
    SUBCASE("LRU: 1 hit, 5 misses, 2 evictions")
    {
        const IoStats stats = runTrace(dir.path(), kSpecTrace, 3, ReplacementPolicy::Lru);
        CHECK(stats == IoStats{1, 5, 2, 5, 0});
    }
    SUBCASE("CLOCK: 2 hits, 4 misses, 1 eviction")
    {
        const IoStats stats = runTrace(dir.path(), kSpecTrace, 3, ReplacementPolicy::Clock);
        CHECK(stats == IoStats{2, 4, 1, 4, 0});
    }
    // The scratch file is gone; tmp/ itself may stay.
    CHECK_FALSE(fs::exists(scratchPath(dir.path())));
}

TEST_CASE("runTrace is deterministic and starts from a fresh pool every call")
{
    TempDir dir("trace_determinism");
    const std::vector<PageId> ids = seededTrace(2000, 100, 7);
    for (const ReplacementPolicy policy : {ReplacementPolicy::Lru, ReplacementPolicy::Clock})
    {
        const IoStats first = runTrace(dir.path(), ids, 16, policy);
        const IoStats second = runTrace(dir.path(), ids, 16, policy);
        CHECK(first == second);
        checkInvariants(first, ids.size());
        // Fresh pool: the spec trace after a big one still gives the spec numbers.
        const IoStats spec = runTrace(dir.path(), kSpecTrace, 3, policy);
        CHECK(spec == (policy == ReplacementPolicy::Lru ? IoStats{1, 5, 2, 5, 0} : IoStats{2, 4, 1, 4, 0}));
    }
    CHECK_FALSE(fs::exists(scratchPath(dir.path())));
}

TEST_CASE("runTrace: sequential flooding, 80 pages scanned 10 times at B = 64")
{
    TempDir dir("trace_flooding");
    const std::vector<PageId> ids = sequentialScans(80, 10);
    REQUIRE(ids.size() == 800);

    // LRU always evicts the page needed soonest (the oldest one, which the
    // scan reaches next), so after the first 64 cold misses every fetch is
    // a miss that replaces a page: 0 hits, 800 misses, 800 - 64 evictions.
    const IoStats lru = runTrace(dir.path(), ids, 64, ReplacementPolicy::Lru);
    CHECK(lru == IoStats{0, 800, 736, 800, 0});

    const IoStats clock = runTrace(dir.path(), ids, 64, ReplacementPolicy::Clock);
    checkInvariants(clock, ids.size());

    // With B >= 80 pages everything fits: 80 cold misses, then hits only.
    const IoStats big = runTrace(dir.path(), ids, 80, ReplacementPolicy::Lru);
    CHECK(big == IoStats{720, 80, 0, 80, 0});
    CHECK_FALSE(fs::exists(scratchPath(dir.path())));
}

TEST_CASE("runTrace: reads == misses and writes == 0 for many traces")
{
    TempDir dir("trace_invariants");
    for (const std::size_t frames : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}})
        for (const ReplacementPolicy policy : {ReplacementPolicy::Lru, ReplacementPolicy::Clock})
        {
            const std::vector<PageId> ids = seededTrace(500, 40, static_cast<std::uint32_t>(frames));
            checkInvariants(runTrace(dir.path(), ids, frames, policy), ids.size());
        }
    // B = 1: every change of page is a miss and, after the first, an eviction.
    const IoStats one = runTrace(dir.path(), {5, 5, 6, 5, 5}, 1, ReplacementPolicy::Lru);
    CHECK(one == IoStats{2, 3, 2, 3, 0});
}

TEST_CASE("runTrace: an empty trace gives all-zero stats")
{
    TempDir dir("trace_empty");
    CHECK(runTrace(dir.path(), {}, 3, ReplacementPolicy::Lru) == IoStats{});
    CHECK(runTrace(dir.path(), {}, 1, ReplacementPolicy::Clock) == IoStats{});
    CHECK_FALSE(fs::exists(scratchPath(dir.path())));
}

TEST_CASE("runTrace: bad arguments throw before any file is made")
{
    TempDir dir("trace_errors");
    CHECK_THROWS_AS(runTrace(dir.path(), kSpecTrace, 0, ReplacementPolicy::Lru), std::logic_error);
    CHECK_THROWS_AS(runTrace(dir.path(), {1, kInvalidPageId}, 3, ReplacementPolicy::Clock), std::logic_error);
    CHECK_FALSE(fs::exists(dir.path() / "tmp"));
}

TEST_CASE("runTrace: a leftover scratch file is replaced, then deleted")
{
    TempDir dir("trace_leftover");
    fs::create_directories(dir.path() / "tmp");
    {
        // 3 bytes: not even a whole page, so reusing it would fail.
        std::ofstream leftover(scratchPath(dir.path()), std::ios::binary);
        leftover << "old";
    }
    // Something else in tmp/ is never touched.
    {
        std::ofstream other(dir.path() / "tmp" / "keep.txt");
        other << "keep";
    }
    CHECK(runTrace(dir.path(), kSpecTrace, 3, ReplacementPolicy::Lru) == IoStats{1, 5, 2, 5, 0});
    CHECK_FALSE(fs::exists(scratchPath(dir.path())));
    CHECK(fs::exists(dir.path() / "tmp" / "keep.txt"));
}

TEST_CASE("runTrace: a leftover that cannot be deleted is an error, and is left alone")
{
    TempDir dir("trace_undeletable");
    // A non-empty folder with the scratch file's name cannot be removed by
    // std::filesystem::remove.
    fs::create_directories(scratchPath(dir.path()));
    {
        std::ofstream inside(scratchPath(dir.path()) / "x.txt");
        inside << "x";
    }
    CHECK_THROWS_AS(runTrace(dir.path(), kSpecTrace, 3, ReplacementPolicy::Lru), std::runtime_error);
    CHECK(fs::exists(scratchPath(dir.path()) / "x.txt"));
}
