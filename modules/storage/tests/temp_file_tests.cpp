// Temp files (chunk 10): counter-based names in <DIR>/tmp/, pages through
// the shared pool (counted like any other), drop discards dirty frames
// without writing, RAII delete on scope exit and on exceptions, move-only,
// drop refused while a page is pinned, stale-file sweep, determinism.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/page_guard.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/temp_file.h"

namespace
{

    namespace fs = std::filesystem;
    using db::storage::BufferPool;
    using db::storage::FileId;
    using db::storage::FileRegistry;
    using db::storage::IoStats;
    using db::storage::PageGuard;
    using db::storage::PageId;
    using db::storage::ReplacementPolicy;
    using db::storage::TempFile;
    using db::storage::TempFileManager;

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

    // Everything a temp-file test needs, in the order it must be built (and
    // destroyed in reverse: manager, then pool, then registry).
    struct Setup
    {
        explicit Setup(const std::string& name, std::size_t frames = 4)
            : dir(name), pool(frames, ReplacementPolicy::Lru, files), temps(dir.path(), files, pool)
        {
        }
        TempDir dir;
        FileRegistry files;
        BufferPool pool;
        TempFileManager temps;
    };

    // Appends one page to `file` whose first byte is `marker`, then unpins it.
    PageId writePage(BufferPool& pool, const TempFile& file, std::byte marker)
    {
        PageGuard guard = PageGuard::newPage(pool, file.fileId());
        guard.data()[0] = marker;
        guard.markDirty();
        return guard.pageId();
    }

    // The first byte of page `pageId` of `file`, read through the pool.
    std::byte firstByte(BufferPool& pool, const TempFile& file, PageId pageId)
    {
        const PageGuard guard = PageGuard::fetch(pool, file.fileId(), pageId);
        return guard.data()[0];
    }

    // The path of a temp file, looked up in the registry while it is open.
    fs::path pathOf(FileRegistry& files, const TempFile& file)
    {
        return files.disk(file.fileId()).path();
    }

    // Makes an empty file at `path` (for the stale-file tests).
    void touch(const fs::path& path)
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
    }

} // namespace

TEST_CASE("temp file: two creates give two ids and two empty counter-named files in tmp/")
{
    Setup s("temp_create");
    TempFile first = s.temps.create();
    TempFile second = s.temps.create();

    CHECK(first.valid());
    CHECK(second.valid());
    CHECK(first.fileId() != second.fileId());
    CHECK(s.temps.liveCount() == 2);
    CHECK(s.temps.folder() == s.dir.path() / "tmp");

    const fs::path one = s.dir.path() / "tmp" / "temp_000001.db";
    const fs::path two = s.dir.path() / "tmp" / "temp_000002.db";
    CHECK(fs::exists(one));
    CHECK(fs::exists(two));
    CHECK(fs::file_size(one) == 0);
    CHECK(fs::file_size(two) == 0);
    CHECK(pathOf(s.files, first).filename() == "temp_000001.db");
    CHECK(first.numPages() == 0);
    // Nothing was read or written to make them.
    CHECK(s.pool.stats() == IoStats{});
}

TEST_CASE("temp file: pages go through the pool, numbered 0, 1, 2, and are counted")
{
    SUBCASE("B = 4: every page stays resident; flush writes 3, reads back are hits")
    {
        Setup s("temp_pages_b4", 4);
        TempFile file = s.temps.create();
        CHECK(writePage(s.pool, file, std::byte{10}) == 0);
        CHECK(writePage(s.pool, file, std::byte{11}) == 1);
        CHECK(writePage(s.pool, file, std::byte{12}) == 2);
        CHECK(file.numPages() == 3);
        // newPage counts nothing (chunk 05 Q2); nothing evicted yet.
        CHECK(s.pool.stats() == IoStats{});

        s.pool.flushAll();
        CHECK(s.pool.stats() == IoStats{0, 0, 0, 0, 3});

        CHECK(firstByte(s.pool, file, 0) == std::byte{10});
        CHECK(firstByte(s.pool, file, 1) == std::byte{11});
        CHECK(firstByte(s.pool, file, 2) == std::byte{12});
        CHECK(s.pool.stats() == IoStats{3, 0, 0, 0, 3});
    }

    SUBCASE("B = 2: the third page evicts dirty page 0 (one write); reading it back is a miss")
    {
        Setup s("temp_pages_b2", 2);
        TempFile file = s.temps.create();
        writePage(s.pool, file, std::byte{10});
        writePage(s.pool, file, std::byte{11});
        writePage(s.pool, file, std::byte{12});
        CHECK(s.pool.stats() == IoStats{0, 0, 0, 0, 1});

        // Page 0 was written at eviction, so its content survived on disk.
        CHECK(firstByte(s.pool, file, 0) == std::byte{10});
        // Miss + read; it replaced dirty page 1 (eviction + write).
        CHECK(s.pool.stats() == IoStats{0, 1, 1, 1, 2});
        CHECK(s.pool.peakPinnedCount() <= 1);
    }
}

TEST_CASE("temp file: drop discards dirty resident pages without a write and deletes the file")
{
    Setup s("temp_drop_dirty");
    TempFile keep = s.temps.create();
    TempFile file = s.temps.create();
    writePage(s.pool, file, std::byte{1});
    writePage(s.pool, file, std::byte{2});
    const fs::path path = pathOf(s.files, file);
    const FileId id = file.fileId();
    REQUIRE(s.pool.isResident(id, 0));
    REQUIRE(s.temps.liveCount() == 2);

    const IoStats before = s.pool.stats();
    file.drop();

    CHECK(s.pool.stats() == before); // no write for the two dirty pages
    CHECK_FALSE(file.valid());
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(s.files.contains(id));
    CHECK_FALSE(s.pool.isResident(id, 0));
    CHECK_FALSE(s.pool.isResident(id, 1));
    CHECK(s.pool.pinnedFrameCount() == 0);
    CHECK(s.temps.liveCount() == 1);
    CHECK(keep.valid()); // the other file is untouched

    // A second drop does nothing.
    file.drop();
    CHECK(s.temps.liveCount() == 1);
}

TEST_CASE("temp file: dropped frames are free again, so reusing them is not an eviction")
{
    Setup s("temp_drop_frees", 2);
    TempFile file = s.temps.create();
    writePage(s.pool, file, std::byte{1});
    writePage(s.pool, file, std::byte{2});
    file.drop();

    TempFile other = s.temps.create();
    writePage(s.pool, other, std::byte{3});
    writePage(s.pool, other, std::byte{4});
    CHECK(s.pool.stats() == IoStats{});
}

TEST_CASE("temp file: going out of scope without drop deletes the file")
{
    Setup s("temp_scope");
    fs::path path;
    {
        TempFile file = s.temps.create();
        writePage(s.pool, file, std::byte{1});
        path = pathOf(s.files, file);
        REQUIRE(fs::exists(path));
        REQUIRE(s.temps.liveCount() == 1);
    }
    CHECK_FALSE(fs::exists(path));
    CHECK(s.temps.liveCount() == 0);
    CHECK(s.pool.stats() == IoStats{});
}

TEST_CASE("temp file: an exception while a temp file is alive still deletes it")
{
    Setup s("temp_exception");
    fs::path path;
    const auto failingSort = [&] {
        TempFile run = s.temps.create();
        writePage(s.pool, run, std::byte{1});
        path = pathOf(s.files, run);
        throw std::runtime_error("sort failed halfway");
    };
    CHECK_THROWS_AS(failingSort(), std::runtime_error);

    CHECK_FALSE(path.empty());
    CHECK_FALSE(fs::exists(path));
    CHECK(s.temps.liveCount() == 0);
    CHECK(s.pool.pinnedFrameCount() == 0);
}

TEST_CASE("temp file: moves hand the file over and it is deleted exactly once")
{
    Setup s("temp_move");

    SUBCASE("move constructor")
    {
        TempFile source = s.temps.create();
        const FileId id = source.fileId();
        const fs::path path = pathOf(s.files, source);

        TempFile target(std::move(source));
        CHECK_FALSE(source.valid()); // NOLINT(bugprone-use-after-move): testing the moved-from state
        CHECK_THROWS_AS(static_cast<void>(source.fileId()), std::logic_error);
        CHECK(target.valid());
        CHECK(target.fileId() == id);
        CHECK(s.temps.liveCount() == 1);

        source.drop(); // empty: does nothing
        CHECK(fs::exists(path));
        CHECK(s.temps.liveCount() == 1);

        target.drop();
        CHECK_FALSE(fs::exists(path));
        CHECK(s.temps.liveCount() == 0);
    }

    SUBCASE("move assignment drops the target's own file first")
    {
        TempFile target = s.temps.create();
        TempFile source = s.temps.create();
        const fs::path oldPath = pathOf(s.files, target);
        const fs::path newPath = pathOf(s.files, source);
        const FileId newId = source.fileId();

        target = std::move(source);
        CHECK_FALSE(fs::exists(oldPath));
        CHECK(fs::exists(newPath));
        CHECK(target.fileId() == newId);
        CHECK_FALSE(source.valid()); // NOLINT(bugprone-use-after-move)
        CHECK(s.temps.liveCount() == 1);
    }

    SUBCASE("moving into an empty handle, then letting both go out of scope")
    {
        fs::path path;
        {
            TempFile target;
            CHECK_FALSE(target.valid());
            TempFile source = s.temps.create();
            path = pathOf(s.files, source);
            target = std::move(source);
            CHECK(s.temps.liveCount() == 1);
        }
        CHECK_FALSE(fs::exists(path));
        CHECK(s.temps.liveCount() == 0);
    }
}

TEST_CASE("temp file: drop while a page is pinned throws and changes nothing")
{
    Setup s("temp_pinned");
    TempFile file = s.temps.create();
    const fs::path path = pathOf(s.files, file);
    PageGuard guard = PageGuard::newPage(s.pool, file.fileId());

    CHECK_THROWS_AS(file.drop(), std::logic_error);
    CHECK(file.valid());
    CHECK(fs::exists(path));
    CHECK(s.temps.liveCount() == 1);
    CHECK(s.pool.isResident(file.fileId(), 0));

    // Following the rule "release guards first", the drop works.
    guard.release();
    file.drop();
    CHECK_FALSE(fs::exists(path));
    CHECK(s.temps.liveCount() == 0);
}

TEST_CASE("temp file: an empty handle refuses fileId and numPages")
{
    TempFile empty;
    CHECK_FALSE(empty.valid());
    CHECK_THROWS_AS(static_cast<void>(empty.fileId()), std::logic_error);
    CHECK_THROWS_AS(static_cast<void>(empty.numPages()), std::logic_error);
    empty.drop(); // does nothing
}

TEST_CASE("temp file: a fresh manager in a fresh folder gives the same names in the same order")
{
    const auto names = [](const std::string& folder) {
        Setup s(folder);
        std::vector<std::string> result;
        TempFile a = s.temps.create();
        TempFile b = s.temps.create();
        result.push_back(pathOf(s.files, a).filename().string());
        b.drop();
        TempFile c = s.temps.create(); // the counter never goes back
        result.push_back(pathOf(s.files, c).filename().string());
        return result;
    };
    const std::vector<std::string> expected{"temp_000001.db", "temp_000003.db"};
    CHECK(names("temp_determinism_1") == expected);
    CHECK(names("temp_determinism_2") == expected);
}

TEST_CASE("temp file: a new manager sweeps stale temp files and leaves everything else")
{
    TempDir dir("temp_stale");
    const fs::path tmp = dir.path() / "tmp";
    fs::create_directories(tmp);
    touch(tmp / "temp_000001.db");     // stale: would block the first create
    touch(tmp / "temp_000042.db");     // stale
    touch(tmp / "notes.txt");          // not ours
    touch(tmp / "temp_abc.db");        // not our pattern
    touch(tmp / "temp_.db");           // no digits: not our pattern
    touch(dir.path() / "temp_000002.db"); // outside tmp/: never touched

    FileRegistry files;
    BufferPool pool(4, ReplacementPolicy::Lru, files);
    TempFileManager temps(dir.path(), files, pool);

    CHECK_FALSE(fs::exists(tmp / "temp_000001.db"));
    CHECK_FALSE(fs::exists(tmp / "temp_000042.db"));
    CHECK(fs::exists(tmp / "notes.txt"));
    CHECK(fs::exists(tmp / "temp_abc.db"));
    CHECK(fs::exists(tmp / "temp_.db"));
    CHECK(fs::exists(dir.path() / "temp_000002.db"));

    TempFile file = temps.create();
    CHECK(files.disk(file.fileId()).path().filename() == "temp_000001.db");
}

TEST_CASE("temp file: no tmp/ folder yet is fine; it is created on first create")
{
    Setup s("temp_no_folder");
    CHECK_FALSE(fs::exists(s.dir.path() / "tmp"));
    TempFile file = s.temps.create();
    CHECK(fs::is_directory(s.dir.path() / "tmp"));
}

TEST_CASE("temp file: a failed create moves neither the counter nor the live count")
{
    Setup s("temp_create_fails");
    TempFile first = s.temps.create();
    // Something else now holds the next name (not a stale file: it appeared
    // after the sweep), so create must fail with the registry's error.
    touch(s.temps.folder() / "temp_000002.db");
    CHECK_THROWS(s.temps.create());
    CHECK(s.temps.liveCount() == 1);

    fs::remove(s.temps.folder() / "temp_000002.db");
    TempFile second = s.temps.create();
    CHECK(pathOf(s.files, second).filename() == "temp_000002.db");
    CHECK(s.temps.liveCount() == 2);
}
