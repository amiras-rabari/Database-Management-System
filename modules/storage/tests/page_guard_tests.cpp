// PageGuard (RAII pin: scope exit, exceptions, move construction and
// assignment, markDirty, release, empty guards, newPage) and IoStats
// difference and sum.

#include <array>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <doctest/doctest.h>

#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/page_guard.h"
#include "db/storage/replacement_policy.h"

namespace
{

    namespace fs = std::filesystem;
    using db::storage::BufferPool;
    using db::storage::FileId;
    using db::storage::FileRegistry;
    using db::storage::IoStats;
    using db::storage::kPageSize;
    using db::storage::PageGuard;
    using db::storage::PageId;
    using db::storage::ReplacementPolicy;
    using Page = std::array<std::byte, kPageSize>;

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

    // A temp folder, a registry, one file with 8 zero pages and a pool of
    // `frames` frames. Members are created in this order.
    struct Fixture
    {
        TempDir dir;
        FileRegistry files;
        FileId file;
        BufferPool pool;

        Fixture(const std::string& name, std::size_t frames)
            : dir(name), file(files.create(dir.path() / "t.db")), pool(frames, ReplacementPolicy::Lru, files)
        {
            for (PageId i = 0; i < 8; ++i)
                files.disk(file).allocatePage();
        }

        Page onDisk(PageId pageId)
        {
            Page page{};
            files.disk(file).readPage(pageId, page);
            return page;
        }
    };

} // namespace

// ------------------------------------------------------------------ guard

TEST_CASE("guard: the pin lasts exactly as long as the guard's scope")
{
    Fixture fx("guard_scope", 2);
    {
        const PageGuard guard = PageGuard::fetch(fx.pool, fx.file, 3);
        CHECK(guard.valid());
        CHECK(guard.fileId() == fx.file);
        CHECK(guard.pageId() == 3);
        CHECK(guard.data().size() == kPageSize);
        CHECK(fx.pool.pinnedFrameCount() == 1);
    }
    CHECK(fx.pool.pinnedFrameCount() == 0);
    CHECK(fx.pool.isResident(fx.file, 3)); // unpinned, not evicted
}

TEST_CASE("guard: an exception thrown while a guard is alive still unpins")
{
    Fixture fx("guard_exception", 2);
    CHECK_THROWS_AS(
        [&] {
            const PageGuard guard = PageGuard::fetch(fx.pool, fx.file, 0);
            throw std::runtime_error("work failed");
        }(),
        std::runtime_error);
    CHECK(fx.pool.pinnedFrameCount() == 0);
}

TEST_CASE("guard: a failed fetch leaves no guard and no pin")
{
    Fixture fx("guard_failed_fetch", 1);
    const PageGuard held = PageGuard::fetch(fx.pool, fx.file, 0);
    CHECK_THROWS_AS(PageGuard::fetch(fx.pool, fx.file, 1), std::logic_error); // no unpinned frame
    CHECK(fx.pool.pinnedFrameCount() == 1);
}

TEST_CASE("guard: move construction hands over the pin, released exactly once")
{
    Fixture fx("guard_move_construct", 2);
    {
        PageGuard first = PageGuard::fetch(fx.pool, fx.file, 0);
        PageGuard second(std::move(first));
        CHECK(second.valid());
        CHECK(second.pageId() == 0);
        CHECK_FALSE(first.valid()); // NOLINT(bugprone-use-after-move): checking the moved-from state
        CHECK(fx.pool.pinnedFrameCount() == 1);
    }
    // Both destructors ran; a second unpin would have thrown inside the pool.
    CHECK(fx.pool.pinnedFrameCount() == 0);
    CHECK_THROWS_AS(fx.pool.unpinPage(fx.file, 0, false), std::logic_error); // pin count already 0
}

TEST_CASE("guard: move assignment unpins the old page at once")
{
    Fixture fx("guard_move_assign", 3);
    PageGuard a = PageGuard::fetch(fx.pool, fx.file, 0);
    PageGuard b = PageGuard::fetch(fx.pool, fx.file, 1);
    CHECK(fx.pool.pinnedFrameCount() == 2);

    a = std::move(b);
    CHECK(fx.pool.pinnedFrameCount() == 1); // page 0 released, page 1 kept
    CHECK(a.pageId() == 1);
    CHECK_FALSE(b.valid()); // NOLINT(bugprone-use-after-move)
    CHECK_THROWS_AS(fx.pool.unpinPage(fx.file, 0, false), std::logic_error);

    // Assigning onto an empty guard just takes the pin.
    PageGuard empty;
    empty = std::move(a);
    CHECK(empty.pageId() == 1);
    CHECK(fx.pool.pinnedFrameCount() == 1);

    // Self-assignment (through a reference, as real code would reach it)
    // keeps the pin.
    PageGuard& alias = empty;
    empty = std::move(alias);
    CHECK(empty.valid());
    CHECK(fx.pool.pinnedFrameCount() == 1);
}

TEST_CASE("guard: markDirty makes the eviction write the page")
{
    Fixture fx("guard_mark_dirty", 1);
    {
        PageGuard guard = PageGuard::fetch(fx.pool, fx.file, 0);
        guard.data()[0] = std::byte{0x5A};
        guard.markDirty();
    }
    { const PageGuard next = PageGuard::fetch(fx.pool, fx.file, 1); } // evicts page 0
    CHECK(fx.pool.stats().writes == 1);
    CHECK(fx.onDisk(0)[0] == std::byte{0x5A});
}

TEST_CASE("guard: without markDirty the change is not written")
{
    Fixture fx("guard_not_dirty", 1);
    {
        PageGuard guard = PageGuard::fetch(fx.pool, fx.file, 0);
        guard.data()[0] = std::byte{0x5A}; // changed, but not reported
    }
    { const PageGuard next = PageGuard::fetch(fx.pool, fx.file, 1); }
    CHECK(fx.pool.stats().writes == 0);
    CHECK(fx.onDisk(0)[0] == std::byte{0});
}

TEST_CASE("guard: release unpins now, and a second release is harmless")
{
    Fixture fx("guard_release", 2);
    PageGuard guard = PageGuard::fetch(fx.pool, fx.file, 0);
    guard.release();
    CHECK_FALSE(guard.valid());
    CHECK(fx.pool.pinnedFrameCount() == 0);
    guard.release();
    CHECK(fx.pool.pinnedFrameCount() == 0);
}

TEST_CASE("guard: using an empty guard is a logic_error")
{
    Fixture fx("guard_empty", 2);
    PageGuard empty;
    CHECK_FALSE(empty.valid());
    CHECK_THROWS_AS(empty.data(), std::logic_error);
    CHECK_THROWS_AS(std::as_const(empty).data(), std::logic_error);
    CHECK_THROWS_AS(empty.fileId(), std::logic_error);
    CHECK_THROWS_AS(empty.pageId(), std::logic_error);
    CHECK_THROWS_AS(empty.markDirty(), std::logic_error);
    empty.release(); // fine: nothing to release

    PageGuard released = PageGuard::fetch(fx.pool, fx.file, 0);
    released.release();
    CHECK_THROWS_AS(released.data(), std::logic_error);
}

TEST_CASE("guard: newPage gives a zero page that flushAll writes")
{
    Fixture fx("guard_new_page", 2);
    {
        PageGuard guard = PageGuard::newPage(fx.pool, fx.file);
        CHECK(guard.pageId() == 8);
        bool allZero = true;
        for (std::byte b : guard.data())
            allZero = allZero && b == std::byte{0};
        CHECK(allZero);
        guard.data()[7] = std::byte{0x42};
        CHECK(fx.pool.pinnedFrameCount() == 1);
    } // no markDirty: the pool made the new page dirty itself
    fx.pool.flushAll();
    CHECK(fx.pool.stats().writes == 1);
    CHECK(fx.onDisk(8)[7] == std::byte{0x42});
}

// ---------------------------------------------------------------- IoStats

TEST_CASE("stats: a snapshot difference measures one piece of work")
{
    Fixture fx("stats_difference", 4);
    { const PageGuard warm = PageGuard::fetch(fx.pool, fx.file, 7); } // before the measured work

    const IoStats before = fx.pool.stats();
    for (PageId page : {0u, 1u, 2u})
        const PageGuard guard = PageGuard::fetch(fx.pool, fx.file, page);
    const IoStats middle = fx.pool.stats();
    { const PageGuard again = PageGuard::fetch(fx.pool, fx.file, 1); } // a hit
    const IoStats after = fx.pool.stats();

    CHECK(middle - before == IoStats{0, 3, 0, 3, 0});
    CHECK(after - middle == IoStats{1, 0, 0, 0, 0});
    // The parts add up to the whole interval.
    CHECK((middle - before) + (after - middle) == after - before);
    CHECK(after - before == IoStats{1, 3, 0, 3, 0});
    // A snapshot is a copy: later activity does not change it.
    CHECK(before == IoStats{0, 1, 0, 1, 0});
}

TEST_CASE("stats: sum and difference are field by field")
{
    const IoStats a{1, 2, 3, 4, 5};
    const IoStats b{10, 20, 30, 40, 50};
    CHECK(a + b == IoStats{11, 22, 33, 44, 55});
    CHECK(b - a == IoStats{9, 18, 27, 36, 45});
    CHECK(a - a == IoStats{});
}
