// Heap file (SPEC §M1.5): create, first-fit insert, get, remove,
// numDataPages, directory growth past 681 data pages, the "record too long"
// and "no record at rid" errors (§M1.8), and the two-pin limit.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/directory_page.h"
#include "db/storage/file_registry.h"
#include "db/storage/heap_file.h"
#include "db/storage/page.h"
#include "db/storage/page_guard.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/rid.h"
#include "db/storage/slotted_page.h"

namespace
{

    namespace fs = std::filesystem;
    using db::Column;
    using db::Error;
    using db::ErrorCategory;
    using db::Schema;
    using db::Tuple;
    using db::Type;
    using db::Value;
    using db::storage::BufferPool;
    using db::storage::DirectoryEntry;
    using db::storage::DirectoryPage;
    using db::storage::FileId;
    using db::storage::FileRegistry;
    using db::storage::HeapFile;
    using db::storage::kInvalidPageId;
    using db::storage::PageGuard;
    using db::storage::PageId;
    using db::storage::ReplacementPolicy;
    using db::storage::Rid;
    using db::storage::SlottedPage;

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

    // The §M1.3 example schema (a INT, b VARCHAR(5), c INT) of table R.
    Schema exampleSchema()
    {
        return Schema({Column{"R", "a", Type::Int, 0}, Column{"R", "b", Type::Varchar, 5},
            Column{"R", "c", Type::Int, 0}});
    }

    // (a, b, 7). With b = "bob" the record is 13 bytes (§M1.3); with b = ""
    // it is 10 bytes.
    Tuple row(std::int32_t a, const std::string& b)
    {
        return Tuple{Value::ofInt(a), Value::ofVarchar(b), Value::ofInt(7)};
    }

    // 16 VARCHAR(255) columns: records from 32 to 16 * 257 = 4112 bytes, so
    // both sides of the 4084-byte limit can be built.
    constexpr int kWideColumns = 16;

    Schema wideSchema()
    {
        std::vector<Column> columns;
        for (int i = 0; i < kWideColumns; ++i)
            columns.push_back(Column{"R", "v" + std::to_string(i), Type::Varchar, 255});
        return Schema(columns);
    }

    // A wide-schema tuple whose record is exactly `recordLength` bytes:
    // 2 length bytes per column, and the rest spread over the columns,
    // up to 255 each. `fill` picks the letter, so rows can differ.
    Tuple wideRow(std::uint32_t recordLength, char fill = 'x')
    {
        std::uint32_t textLeft = recordLength - 2 * kWideColumns;
        Tuple tuple;
        for (int i = 0; i < kWideColumns; ++i)
        {
            const std::uint32_t here = textLeft < 255 ? textLeft : 255;
            tuple.push_back(Value::ofVarchar(std::string(here, fill)));
            textLeft -= here;
        }
        return tuple;
    }

    // Makes `file` (empty) into heap file "R" and opens it.
    HeapFile createHeap(BufferPool& pool, FileId file, Schema schema)
    {
        HeapFile::create(pool, file);
        return HeapFile(pool, file, std::move(schema), "R");
    }

    // A temp folder, a registry, one empty file "R.db" made into a heap
    // file, and a pool of `frames` frames. Members are created in this order.
    struct Fixture
    {
        TempDir dir;
        FileRegistry files;
        FileId file;
        BufferPool pool;
        HeapFile heap;

        Fixture(const std::string& name, std::size_t frames, Schema schema = exampleSchema())
            : dir(name), file(files.create(dir.path() / "R.db")), pool(frames, ReplacementPolicy::Lru, files),
              heap(createHeap(pool, file, std::move(schema)))
        {
        }

        // Directory page `dirPage`'s entry `index`, read through a guard.
        DirectoryEntry entry(PageId dirPage, std::uint16_t index)
        {
            PageGuard guard = PageGuard::fetch(pool, file, dirPage);
            return DirectoryPage(guard.data()).entry(index);
        }

        PageId filePages() { return files.disk(file).numPages(); }
    };

    // The message of the db::Error that `action` throws (checks the category).
    template <typename Action>
    std::string errorMessage(Action action, ErrorCategory category)
    {
        try
        {
            action();
        }
        catch (const Error& e)
        {
            CHECK(e.category() == category);
            return e.message();
        }
        FAIL("no db::Error thrown");
        return "";
    }

    // Every test below runs with B = 4 and again with B = 2 (two pins is all
    // an insert needs).
    const std::size_t kFrameCounts[] = {4, 2};

} // namespace

TEST_CASE("Rid: equality, order (page first, then slot) and text")
{
    CHECK(Rid{3, 7} == Rid{3, 7});
    CHECK(Rid{3, 7} != Rid{3, 8});
    CHECK(Rid{2, 9} < Rid{3, 0});
    CHECK(Rid{3, 0} < Rid{3, 1});
    CHECK(Rid{3, 7}.toString() == "(3, 7)");
}

TEST_CASE("HeapFile::create: page 0 is an empty directory page with no next page")
{
    Fixture f("heap_create", 4);
    CHECK(f.filePages() == 1);
    CHECK(f.heap.numDataPages() == 0);
    PageGuard guard = PageGuard::fetch(f.pool, f.file, 0);
    DirectoryPage dir(guard.data());
    CHECK(dir.pageId() == 0);
    CHECK(dir.nextDirPage() == kInvalidPageId);
    CHECK(dir.entryCount() == 0);
}

TEST_CASE("HeapFile::create on a file that already has pages is a caller bug")
{
    Fixture f("heap_create_twice", 4);
    CHECK_THROWS_AS(HeapFile::create(f.pool, f.file), std::logic_error);
    CHECK(f.filePages() == 1);
}

TEST_CASE("HeapFile: first insert goes to rid (1, 0); entry 0 is {1, 4071} (§M1.4 example)")
{
    for (std::size_t frames : kFrameCounts)
    {
        CAPTURE(frames);
        Fixture f("heap_first_insert", frames);
        CHECK(f.heap.insert(row(42, "bob")) == Rid{1, 0});
        CHECK(f.entry(0, 0) == DirectoryEntry{1, 4071});
        CHECK(f.heap.numDataPages() == 1);
        CHECK(f.heap.get(Rid{1, 0}) == row(42, "bob"));
        CHECK(f.pool.pinnedFrameCount() == 0);
    }
}

TEST_CASE("HeapFile: 240 records of 13 bytes fill page 1; the 241st goes to (2, 0)")
{
    for (std::size_t frames : kFrameCounts)
    {
        CAPTURE(frames);
        Fixture f("heap_capacity", frames);
        for (int i = 0; i < 240; ++i)
            REQUIRE(f.heap.insert(row(i, "bob")) == Rid{1, static_cast<std::uint16_t>(i)});
        // 4088 - 240 * 17 = 8 bytes left: less than the 17 a record needs.
        CHECK(f.entry(0, 0) == DirectoryEntry{1, 8});
        CHECK(f.heap.insert(row(240, "bob")) == Rid{2, 0});
        CHECK(f.heap.numDataPages() == 2);
        CHECK(f.entry(0, 1) == DirectoryEntry{2, 4071});

        // Every record comes back unchanged.
        for (int i = 0; i < 240; ++i)
            REQUIRE(f.heap.get(Rid{1, static_cast<std::uint16_t>(i)}) == row(i, "bob"));
        CHECK(f.heap.get(Rid{2, 0}) == row(240, "bob"));
        CHECK(f.pool.pinnedFrameCount() == 0);
    }
}

TEST_CASE("HeapFile: first fit takes the earliest page with free_bytes >= L + 4, even an exact fit")
{
    for (std::size_t frames : kFrameCounts)
    {
        CAPTURE(frames);
        Fixture f("heap_first_fit", frames);
        // 238 * 17 + 2 * 14 = 4074 bytes used: page 1 keeps exactly 14 free.
        for (int i = 0; i < 238; ++i)
            f.heap.insert(row(i, "bob"));
        f.heap.insert(row(238, ""));
        f.heap.insert(row(239, ""));
        REQUIRE(f.entry(0, 0) == DirectoryEntry{1, 14});

        // 13 bytes need 17 > 14: a new page 2.
        CHECK(f.heap.insert(row(240, "bob")) == Rid{2, 0});
        // 10 bytes need exactly 14: page 1 is first and fits (>=, not >).
        CHECK(f.heap.insert(row(241, "")) == Rid{1, 240});
        CHECK(f.entry(0, 0) == DirectoryEntry{1, 0});
        CHECK(f.heap.get(Rid{1, 240}) == row(241, ""));
    }
}

TEST_CASE("HeapFile: remove makes the rid an error naming it; other rids still work; free bytes unchanged")
{
    for (std::size_t frames : kFrameCounts)
    {
        CAPTURE(frames);
        Fixture f("heap_remove", frames);
        for (int i = 0; i < 10; ++i)
            f.heap.insert(row(i, "bob"));
        const DirectoryEntry before = f.entry(0, 0);

        f.heap.remove(Rid{1, 5});
        const std::string getError = errorMessage([&] { f.heap.get(Rid{1, 5}); }, ErrorCategory::Name);
        CHECK(getError.find("(1, 5)") != std::string::npos);
        CHECK(getError.find("'R'") != std::string::npos);
        const std::string removeError = errorMessage([&] { f.heap.remove(Rid{1, 5}); }, ErrorCategory::Name);
        CHECK(removeError.find("(1, 5)") != std::string::npos);

        CHECK(f.heap.get(Rid{1, 4}) == row(4, "bob"));
        CHECK(f.heap.get(Rid{1, 6}) == row(6, "bob"));
        // No compaction (D3): the deleted 13 bytes are not given back.
        CHECK(f.entry(0, 0) == before);
        // The slot is never reused: the next insert gets slot 10.
        CHECK(f.heap.insert(row(10, "bob")) == Rid{1, 10});
        CHECK(f.pool.pinnedFrameCount() == 0);
    }
}

TEST_CASE("HeapFile: a rid that names no data page or no slot is an error naming it")
{
    Fixture f("heap_bad_rid", 4);
    f.heap.insert(row(1, "bob"));
    // Page 0 is a directory page; page 9 is past the end of the file.
    CHECK(errorMessage([&] { f.heap.get(Rid{0, 0}); }, ErrorCategory::Name).find("(0, 0)") != std::string::npos);
    CHECK(errorMessage([&] { f.heap.get(Rid{9, 0}); }, ErrorCategory::Name).find("(9, 0)") != std::string::npos);
    CHECK(errorMessage([&] { f.heap.get(Rid{1, 1}); }, ErrorCategory::Name).find("(1, 1)") != std::string::npos);
    CHECK(errorMessage([&] { f.heap.remove(Rid{0, 0}); }, ErrorCategory::Name).find("(0, 0)") != std::string::npos);
    CHECK(errorMessage([&] { f.heap.remove(Rid{1, 1}); }, ErrorCategory::Name).find("(1, 1)") != std::string::npos);
    CHECK(f.pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapFile: 4084 bytes is the largest record; 4085 is 'record too long' naming the table, nothing changed")
{
    for (std::size_t frames : kFrameCounts)
    {
        CAPTURE(frames);
        Fixture f("heap_too_long", frames, wideSchema());

        const std::string message =
            errorMessage([&] { f.heap.insert(wideRow(4085)); }, ErrorCategory::Schema);
        CHECK(message.find("'R'") != std::string::npos);
        CHECK(message.find("4085") != std::string::npos);
        // The row's values are in the message too.
        CHECK(message.find(std::string(255, 'x')) != std::string::npos);
        CHECK(f.filePages() == 1); // no page was added
        CHECK(f.pool.pinnedFrameCount() == 0);

        // 4084 + 4 = 4088 = all the free space of an empty page.
        CHECK(f.heap.insert(wideRow(4084)) == Rid{1, 0});
        CHECK(f.entry(0, 0) == DirectoryEntry{1, 0});
        CHECK(f.heap.get(Rid{1, 0}) == wideRow(4084));
    }
}

TEST_CASE("HeapFile: a tuple that does not match the schema throws the codec's error and changes nothing")
{
    Fixture f("heap_bad_tuple", 4);
    CHECK_THROWS_AS(f.heap.insert(Tuple{Value::ofInt(1)}), Error);
    CHECK(f.filePages() == 1);
}

TEST_CASE("HeapFile: 700 data pages need a second directory page at 682; never more than 2 pins (B = 2)")
{
    Fixture f("heap_directory_growth", 2, wideSchema());
    f.pool.resetPeak();
    // 2000-byte records: 2 per page (2 * 2004 = 4008 <= 4088, 3 do not fit).
    std::vector<Rid> rids;
    for (int i = 0; i < 1400; ++i)
        rids.push_back(f.heap.insert(wideRow(2000, static_cast<char>('a' + i % 26))));
    CHECK(f.pool.peakPinnedCount() <= 2);
    CHECK(f.pool.pinnedFrameCount() == 0);

    // Pages: dir 0, data 1..681, dir 682, data 683..701 -> 702 pages, 700 data pages.
    CHECK(f.filePages() == 702);
    CHECK(f.heap.numDataPages() == 700);
    CHECK(rids[1361] == Rid{681, 1});
    CHECK(rids[1362] == Rid{683, 0});
    CHECK(rids[1399] == Rid{701, 1});
    {
        PageGuard first = PageGuard::fetch(f.pool, f.file, 0);
        DirectoryPage dir(first.data());
        CHECK(dir.nextDirPage() == 682);
        CHECK(dir.entryCount() == 681);
        CHECK(dir.entry(680) == DirectoryEntry{681, 80});
    }
    {
        PageGuard second = PageGuard::fetch(f.pool, f.file, 682);
        DirectoryPage dir(second.data());
        CHECK(dir.pageId() == 682);
        CHECK(dir.nextDirPage() == kInvalidPageId);
        CHECK(dir.entryCount() == 19);
        CHECK(dir.entry(0) == DirectoryEntry{683, 80});
        CHECK(dir.entry(18) == DirectoryEntry{701, 80});
    }
    {
        // The first data page after the second directory page carries its own id.
        PageGuard data = PageGuard::fetch(f.pool, f.file, 683);
        CHECK(SlottedPage(data.data()).pageId() == 683);
    }

    // Every rid reads back, and a directory page id is not a data page.
    for (int i = 0; i < 1400; ++i)
        REQUIRE(f.heap.get(rids[i]) == wideRow(2000, static_cast<char>('a' + i % 26)));
    CHECK_THROWS_AS(f.heap.get(Rid{682, 0}), Error);

    // remove on a page listed by the second directory page finds its entry there.
    f.pool.resetPeak();
    f.heap.remove(rids[1399]);
    CHECK_THROWS_AS(f.heap.get(rids[1399]), Error);
    CHECK(f.pool.peakPinnedCount() <= 2);
    CHECK(f.pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapFile: a page the directory does not list makes the next new data page an error")
{
    // A stray page 1 (e.g. allocated, but the directory change never flushed).
    // The next insert gets data page 2, but entry 0 of page 0 must be page 1.
    Fixture f("heap_stray_page", 4);
    f.files.disk(f.file).allocatePage();
    CHECK_THROWS_AS(f.heap.insert(row(1, "bob")), std::runtime_error);
    CHECK(f.pool.pinnedFrameCount() == 0);
    // The check runs before addEntry: the directory still lists nothing.
    PageGuard guard = PageGuard::fetch(f.pool, f.file, 0);
    CHECK(DirectoryPage(guard.data()).entryCount() == 0);
}

TEST_CASE("HeapFile: a stray page at a directory boundary is caught at the first data page after it")
{
    // 4084-byte records: one per page, so 681 inserts fill directory page 0.
    // A stray page 682 pushes the new directory page to 683 and its first
    // data page to 684, which the arithmetic places on directory page 682.
    Fixture f("heap_stray_boundary", 2, wideSchema());
    for (int i = 0; i < 681; ++i)
        f.heap.insert(wideRow(4084));
    REQUIRE(f.filePages() == 682);
    f.files.disk(f.file).allocatePage();
    CHECK_THROWS_AS(f.heap.insert(wideRow(4084)), std::runtime_error);
    CHECK(f.pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapFile: a new data page at the right entry number but in another directory block is caught")
{
    // 682 stray pages (1..682): the next data page is 683, which is entry 0
    // (the entry number page 0 expects) but of directory page 682, not page 0.
    Fixture f("heap_stray_block", 4);
    for (int i = 0; i < 682; ++i)
        f.files.disk(f.file).allocatePage();
    CHECK_THROWS_AS(f.heap.insert(row(1, "bob")), std::runtime_error);
    CHECK(f.pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapFile: records survive flushAll and a new pool")
{
    TempDir dir("heap_reopen");
    FileRegistry files;
    const FileId file = files.create(dir.path() / "R.db");
    {
        BufferPool pool(4, ReplacementPolicy::Lru, files);
        HeapFile::create(pool, file);
        HeapFile heap(pool, file, exampleSchema(), "R");
        for (int i = 0; i < 300; ++i)
            heap.insert(row(i, "bob"));
        pool.flushAll();
    }
    BufferPool pool(3, ReplacementPolicy::Clock, files);
    HeapFile heap(pool, file, exampleSchema(), "R");
    CHECK(heap.numDataPages() == 2);
    CHECK(heap.get(Rid{1, 239}) == row(239, "bob"));
    CHECK(heap.get(Rid{2, 59}) == row(299, "bob"));
    CHECK(heap.insert(row(300, "bob")) == Rid{2, 60});
}
