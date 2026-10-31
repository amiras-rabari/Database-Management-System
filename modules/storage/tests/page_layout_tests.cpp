// describePage (chunk 15): the §M1.4 worked example row by row through the
// storage engine (including the "delete slot 1" row, decision 15c), the
// directory page 0 description, byte offsets of slots and entries, decoded
// values, errors, and no pin left behind.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/heap_file.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/page_layout.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/rid.h"
#include "db/storage/storage_engine.h"

namespace
{
    namespace fs = std::filesystem;
    using namespace db;
    using namespace db::storage;

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

    // R(a INT, b VARCHAR(20), c INT), bare, as in §M1.4.
    Schema schemaR()
    {
        return Schema({Column{"", "a", Type::Int, 0}, Column{"", "b", Type::Varchar, 20},
            Column{"", "c", Type::Int, 0}});
    }

    Tuple row(int a, const std::string& b, int c)
    {
        return {Value::ofInt(a), Value::ofVarchar(b), Value::ofInt(c)};
    }

    // Checks one slot: number, entry offset 8 + 4i, (offset, length), live.
    void checkSlot(const PageLayout& layout, std::uint16_t i, std::uint16_t offset, std::uint16_t length, bool live)
    {
        REQUIRE(i < layout.slots.size());
        const SlotLayout& slot = layout.slots[i];
        CHECK(slot.slot == i);
        CHECK(slot.entryOffset == 8 + 4 * static_cast<std::size_t>(i));
        CHECK(slot.offset == offset);
        CHECK(slot.length == length);
        CHECK(slot.live == live);
    }

    // Checks a decoded live row (42, 'bob', 7) style.
    void checkValues(const SlotLayout& slot, int a, const std::string& b, int c)
    {
        REQUIRE(slot.values.size() == 3);
        CHECK(slot.values[0].asInt() == a);
        CHECK(slot.values[1].asVarchar() == b);
        CHECK(slot.values[2].asInt() == c);
    }

    template <typename Action>
    Error errorOf(Action action)
    {
        try
        {
            action();
        }
        catch (const Error& error)
        {
            return error;
        }
        FAIL("no db::Error was thrown");
        return Error(ErrorCategory::Lexical, "");
    }
} // namespace

TEST_CASE("describePage reproduces every row of the SPEC M1.4 worked example")
{
    TempDir dir("layout_m14");
    auto engine = StorageEngine::open(dir.path(), 8, ReplacementPolicy::Lru);
    engine->catalog().createTable("R", schemaR());
    HeapFile heap = engine->openHeapFile("R");

    // Row "insert 13 bytes": slot_count 1, free_end 4083, free 4071.
    CHECK(heap.insert(row(42, "bob", 7)) == Rid{1, 0});
    PageLayout layout = describePage(*engine, "R", 1);
    CHECK(layout.kind == PageKind::Data);
    CHECK(layout.table == "R");
    CHECK(layout.pageId == 1);
    CHECK(layout.filePages == 2);
    CHECK(layout.slotCount == 1);
    CHECK(layout.freeEnd == 4083);
    CHECK(layout.freeSpace == 4071);
    REQUIRE(layout.slots.size() == 1);
    checkSlot(layout, 0, 4083, 13, true);
    CHECK(layout.entries.empty());

    // Row "insert 20 bytes": 2, 4063, 4047, slot 1 = (4063, 20).
    CHECK(heap.insert(row(1, "abcdefghij", 2)) == Rid{1, 1});
    layout = describePage(*engine, "R", 1);
    CHECK(layout.slotCount == 2);
    CHECK(layout.freeEnd == 4063);
    CHECK(layout.freeSpace == 4047);
    REQUIRE(layout.slots.size() == 2);
    checkSlot(layout, 0, 4083, 13, true);
    checkSlot(layout, 1, 4063, 20, true);

    // Row "insert 13 bytes": 3, 4050, 4030, slot 2 = (4050, 13).
    CHECK(heap.insert(row(43, "amy", 9)) == Rid{1, 2});
    layout = describePage(*engine, "R", 1);
    CHECK(layout.slotCount == 3);
    CHECK(layout.freeEnd == 4050);
    CHECK(layout.freeSpace == 4030);
    REQUIRE(layout.slots.size() == 3);
    checkSlot(layout, 0, 4083, 13, true);
    checkSlot(layout, 1, 4063, 20, true);
    checkSlot(layout, 2, 4050, 13, true);
    checkValues(layout.slots[0], 42, "bob", 7);
    checkValues(layout.slots[1], 1, "abcdefghij", 2);
    checkValues(layout.slots[2], 43, "amy", 9);
    CHECK(engine->pool().pinnedFrameCount() == 0);

    // Row "delete slot 1": still 3 slots, 4050, 4030; slot 1 = (0, 0), dead.
    heap.remove(Rid{1, 1});
    layout = describePage(*engine, "R", 1);
    CHECK(layout.slotCount == 3);
    CHECK(layout.freeEnd == 4050);
    CHECK(layout.freeSpace == 4030);
    REQUIRE(layout.slots.size() == 3);
    checkSlot(layout, 0, 4083, 13, true);
    checkSlot(layout, 1, 0, 0, false);
    checkSlot(layout, 2, 4050, 13, true);
    CHECK(layout.slots[1].values.empty());
    checkValues(layout.slots[2], 43, "amy", 9);
    CHECK(engine->pool().pinnedFrameCount() == 0);
    engine->close();
}

TEST_CASE("describePage of directory page 0 lists entry 0 -> page 1 with 4030 free bytes")
{
    TempDir dir("layout_directory");
    auto engine = StorageEngine::open(dir.path(), 8, ReplacementPolicy::Lru);
    engine->catalog().createTable("R", schemaR());
    HeapFile heap = engine->openHeapFile("R");

    // Before any row: no entries, no next page.
    PageLayout layout = describePage(*engine, "R", 0);
    CHECK(layout.kind == PageKind::Directory);
    CHECK(layout.pageId == 0);
    CHECK(layout.filePages == 1);
    CHECK(layout.nextDirPage == kInvalidPageId);
    CHECK(layout.entryCount == 0);
    CHECK(layout.entries.empty());

    heap.insert(row(42, "bob", 7));
    heap.insert(row(1, "abcdefghij", 2));
    heap.insert(row(43, "amy", 9));
    layout = describePage(*engine, "R", 0);
    CHECK(layout.kind == PageKind::Directory);
    CHECK(layout.nextDirPage == 0xFFFFFFFFu);
    CHECK(layout.entryCount == 1);
    REQUIRE(layout.entries.size() == 1);
    CHECK(layout.entries[0].index == 0);
    CHECK(layout.entries[0].entryOffset == 10);
    CHECK(layout.entries[0].dataPageId == 1);
    CHECK(layout.entries[0].freeBytes == 4030);
    // The data-page half stays empty.
    CHECK(layout.slotCount == 0);
    CHECK(layout.slots.empty());
    CHECK(engine->pool().pinnedFrameCount() == 0);
    engine->close();
}

TEST_CASE("describePage: directory entries sit at 10 + 6i, data slots at 8 + 4i")
{
    TempDir dir("layout_offsets");
    auto engine = StorageEngine::open(dir.path(), 4, ReplacementPolicy::Lru);
    engine->catalog().createTable("R", schemaR());
    HeapFile heap = engine->openHeapFile("R");
    // 20-character VARCHARs: L = 4 + 22 + 4 = 30, 34 bytes per row with the
    // slot, floor(4088 / 34) = 120 rows per page. 300 rows -> 3 data pages.
    for (int i = 0; i < 300; ++i)
        heap.insert(row(i, "abcdefghijabcdefghij", i));

    const PageLayout dirLayout = describePage(*engine, "R", 0);
    REQUIRE(dirLayout.entries.size() == 3);
    for (std::uint16_t i = 0; i < 3; ++i)
    {
        CHECK(dirLayout.entries[i].index == i);
        CHECK(dirLayout.entries[i].entryOffset == 10 + 6 * static_cast<std::size_t>(i));
        CHECK(dirLayout.entries[i].dataPageId == i + 1u);
    }
    CHECK(dirLayout.entries[0].freeBytes == 4088 - 120 * 34);
    CHECK(dirLayout.entries[2].freeBytes == 4088 - 60 * 34);

    const PageLayout data = describePage(*engine, "R", 2);
    CHECK(data.kind == PageKind::Data);
    CHECK(data.pageId == 2);
    REQUIRE(data.slots.size() == 120);
    CHECK(data.slots[119].entryOffset == 8 + 4 * 119);
    CHECK(data.slots[119].offset == 4096 - 120 * 30);
    checkValues(data.slots[0], 120, "abcdefghijabcdefghij", 120);
    CHECK(engine->pool().pinnedFrameCount() == 0);
    engine->close();
}

TEST_CASE("describePage errors: unknown table, page past the end; no pin left")
{
    TempDir dir("layout_errors");
    auto engine = StorageEngine::open(dir.path(), 4, ReplacementPolicy::Lru);
    engine->catalog().createTable("R", schemaR());
    engine->openHeapFile("R").insert(row(42, "bob", 7));

    const Error unknown = errorOf([&] { describePage(*engine, "S", 0); });
    CHECK(unknown.category() == ErrorCategory::Name);
    CHECK(unknown.message() == "Unknown table 'S'.");

    const Error pastEnd = errorOf([&] { describePage(*engine, "R", 2); });
    CHECK(pastEnd.category() == ErrorCategory::Name);
    CHECK(pastEnd.message() == "Table 'R' has 2 pages; there is no page 2.");
    CHECK(errorOf([&] { describePage(*engine, "R", kInvalidPageId); }).category() == ErrorCategory::Name);
    CHECK(engine->pool().pinnedFrameCount() == 0);

    // One page access: a cold describe is one miss, a second one a hit.
    engine->pool().flushAll();
    const IoStats before = engine->pool().stats();
    describePage(*engine, "R", 1);
    describePage(*engine, "R", 1);
    const IoStats cost = engine->pool().stats() - before;
    CHECK(cost.hits + cost.misses == 2);
    CHECK(cost.writes == 0);
    CHECK(engine->pool().pinnedFrameCount() == 0);
    engine->close();
}
