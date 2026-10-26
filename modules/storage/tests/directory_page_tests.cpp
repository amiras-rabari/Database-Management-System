// Directory page (SPEC §M1.5): header bytes after init, entry bytes, the
// 681-entry capacity ending exactly at byte 4096, chaining, and errors.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include <doctest/doctest.h>

#include "db/storage/bytes.h"
#include "db/storage/directory_page.h"
#include "db/storage/page.h"

namespace
{

    using db::storage::DirectoryEntry;
    using db::storage::DirectoryPage;
    using db::storage::kDirEntriesPerPage;
    using db::storage::kInvalidPageId;
    using db::storage::kPageSize;
    using Page = std::array<std::byte, kPageSize>;

    // The bytes as upper-case hex pairs, e.g. "F8 0F".
    std::string hex(std::span<const std::byte> bytes)
    {
        const char* digits = "0123456789ABCDEF";
        std::string text;
        for (std::byte b : bytes)
        {
            if (!text.empty())
                text += ' ';
            auto value = std::to_integer<unsigned>(b);
            text += digits[value >> 4];
            text += digits[value & 0xF];
        }
        return text;
    }

    // Bytes [from, from + count) of the page.
    std::string hexAt(const Page& page, std::size_t from, std::size_t count)
    {
        return hex(std::span<const std::byte>(page).subspan(from, count));
    }

} // namespace

TEST_CASE("directory: init writes page_id, no next page and zero entries")
{
    Page page{};
    page.fill(std::byte{0xEE}); // init must overwrite the header, whatever was there
    DirectoryPage dir(page);
    dir.init(0);
    CHECK(hexAt(page, 0, 4) == "00 00 00 00");
    CHECK(hexAt(page, 4, 4) == "FF FF FF FF");
    CHECK(hexAt(page, 8, 2) == "00 00");
    CHECK(dir.pageId() == 0);
    CHECK(dir.nextDirPage() == kInvalidPageId);
    CHECK(dir.entryCount() == 0);
    CHECK_FALSE(dir.isFull());
}

TEST_CASE("directory: addEntry and setFreeBytes write the SPEC M1.5 entry bytes")
{
    Page page{};
    DirectoryPage dir(page);
    dir.init(0);

    CHECK(dir.addEntry(1, 4088) == 0);
    CHECK(hexAt(page, 10, 4) == "01 00 00 00"); // data_page_id 1
    CHECK(hexAt(page, 14, 2) == "F8 0F");       // free_bytes 4088
    CHECK(dir.entryCount() == 1);
    CHECK(hexAt(page, 8, 2) == "01 00");
    CHECK(dir.entry(0) == DirectoryEntry{1, 4088});

    // Only bytes 14-15 change.
    const Page beforeUpdate = page;
    dir.setFreeBytes(0, 4071);
    CHECK(hexAt(page, 14, 2) == "E7 0F");
    for (std::size_t i = 0; i < kPageSize; ++i)
        if (i != 14 && i != 15)
            REQUIRE(page[i] == beforeUpdate[i]);
    CHECK(dir.entry(0) == DirectoryEntry{1, 4071});

    // Entry 1 starts 6 bytes later, at 16.
    CHECK(dir.addEntry(2, 100) == 1);
    CHECK(hexAt(page, 16, 6) == "02 00 00 00 64 00");
}

TEST_CASE("directory: 681 entries fit, the last one ends exactly at byte 4096")
{
    CHECK(kDirEntriesPerPage == 681);

    Page page{};
    DirectoryPage dir(page);
    dir.init(0);
    // Entry i = (data page i + 1, free bytes i), so every entry differs.
    for (std::size_t i = 0; i < kDirEntriesPerPage; ++i)
        dir.addEntry(static_cast<std::uint32_t>(i + 1), static_cast<std::uint16_t>(i));
    CHECK(dir.entryCount() == 681);
    CHECK(dir.isFull());

    // Entry 680 = (681, 680) at 10 + 6 * 680 = 4090 .. 4095.
    CHECK(hexAt(page, 4090, 6) == "A9 02 00 00 A8 02");
    CHECK(dir.entry(680) == DirectoryEntry{681, 680});
    CHECK(dir.entry(0) == DirectoryEntry{1, 0});

    CHECK_THROWS_AS(dir.addEntry(999, 0), std::logic_error);
    CHECK(dir.entryCount() == 681);
}

TEST_CASE("directory: setNextDirPage links to the next directory page")
{
    Page page{};
    DirectoryPage dir(page);
    dir.init(0);
    dir.setNextDirPage(682);
    CHECK(hexAt(page, 4, 4) == "AA 02 00 00");
    CHECK(dir.nextDirPage() == 682);
    dir.setNextDirPage(kInvalidPageId);
    CHECK(dir.nextDirPage() == kInvalidPageId);
}

TEST_CASE("directory: errors")
{
    std::array<std::byte, 100> small{};
    CHECK_THROWS_AS(DirectoryPage{small}, std::logic_error);

    Page page{};
    DirectoryPage dir(page);
    dir.init(5);
    dir.addEntry(6, 4088);
    CHECK_THROWS_AS(dir.entry(1), std::logic_error); // entry(entryCount())
    CHECK_THROWS_AS(dir.setFreeBytes(1, 10), std::logic_error);

    // A corrupt page claiming more entries than fit.
    db::storage::writeU16(page, 8, 682);
    CHECK_THROWS_AS(dir.entryCount(), std::runtime_error);
}
