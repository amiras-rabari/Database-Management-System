// Slotted page: the SPEC §M1.4 worked example cell by cell, capacity, get /
// remove rules and the 4084/4088 limit (decision D5).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/storage/bytes.h"
#include "db/storage/page.h"
#include "db/storage/slotted_page.h"

namespace
{

    using db::ErrorCategory;
    using db::storage::kMaxRecordLength;
    using db::storage::kPageSize;
    using db::storage::recordsPerPage;
    using db::storage::SlotEntry;
    using db::storage::SlottedPage;
    using Page = std::array<std::byte, kPageSize>;

    // A record of `length` bytes, every byte `fill`, so records are told apart.
    std::vector<std::byte> record(std::size_t length, unsigned char fill)
    {
        return std::vector<std::byte>(length, static_cast<std::byte>(fill));
    }

    // The bytes as upper-case hex pairs, e.g. "F3 0F".
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

    // Runs `function` and returns the db::Error it threw, or nullopt.
    template <typename Function>
    std::optional<db::Error> errorFrom(Function function)
    {
        try
        {
            function();
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    // Checks one row of the §M1.4 worked-example table.
    void checkRow(const SlottedPage& page, std::uint16_t slotCount, std::uint16_t freeEnd, std::uint16_t freeSpace)
    {
        CHECK(page.slotCount() == slotCount);
        CHECK(page.freeEnd() == freeEnd);
        CHECK(page.freeSpace() == freeSpace);
    }

} // namespace

TEST_CASE("the SPEC M1.4 worked example reproduces exactly")
{
    Page buffer{};
    SlottedPage page(buffer);

    page.init(7);
    CHECK(page.pageId() == 7);
    CHECK(hex(std::span(buffer).first(4)) == "07 00 00 00");
    checkRow(page, 0, 4096, 4088);

    CHECK(page.insert(record(13, 0xA1)) == std::optional<std::uint16_t>(0));
    checkRow(page, 1, 4083, 4071);
    CHECK(page.slot(0) == SlotEntry{4083, 13});
    // Raw bytes: slot_count, free_end and slot 0, little-endian.
    CHECK(hex(std::span(buffer).subspan(4, 2)) == "01 00");
    CHECK(hex(std::span(buffer).subspan(6, 2)) == "F3 0F");
    CHECK(hex(std::span(buffer).subspan(8, 4)) == "F3 0F 0D 00");

    CHECK(page.insert(record(20, 0xB2)) == std::optional<std::uint16_t>(1));
    checkRow(page, 2, 4063, 4047);
    CHECK(page.slot(1) == SlotEntry{4063, 20});

    CHECK(page.insert(record(13, 0xC3)) == std::optional<std::uint16_t>(2));
    checkRow(page, 3, 4050, 4030);
    CHECK(page.slot(2) == SlotEntry{4050, 13});

    page.remove(1);
    checkRow(page, 3, 4050, 4030);
    CHECK(page.slot(1) == SlotEntry{0, 0});
    CHECK_FALSE(page.isLive(1));
    CHECK(page.isLive(0));
    CHECK(page.isLive(2));
    // The other slots are untouched.
    CHECK(page.slot(0) == SlotEntry{4083, 13});
    CHECK(page.slot(2) == SlotEntry{4050, 13});
}

TEST_CASE("capacity: 240 records of 13 bytes fit on one page")
{
    static_assert(recordsPerPage(13) == 240);
    CHECK(recordsPerPage(13) == 240);
    CHECK(recordsPerPage(12) == 255);
    CHECK(recordsPerPage(206) == 19);

    Page buffer{};
    SlottedPage page(buffer);
    page.init(0);
    int inserted = 0;
    while (page.insert(record(13, 0x11)))
        ++inserted;
    CHECK(inserted == 240);
    CHECK(page.slotCount() == 240);
    CHECK(page.freeSpace() < 13 + 4);
}

TEST_CASE("get returns the inserted bytes; a removed slot is a name error")
{
    Page buffer{};
    SlottedPage page(buffer);
    page.init(7);
    const auto first = record(13, 0xA1);
    const auto second = record(20, 0xB2);
    const auto third = record(13, 0xC3);
    page.insert(first);
    page.insert(second);
    page.insert(third);

    CHECK(hex(page.get(1)) == hex(second));
    page.remove(1);

    auto error = errorFrom([&] { page.get(1); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message().find("(7, 1)") != std::string::npos);

    CHECK(hex(page.get(0)) == hex(first));
    CHECK(hex(page.get(2)) == hex(third));
}

TEST_CASE("remove of a dead slot and get/remove past slot_count are name errors")
{
    Page buffer{};
    SlottedPage page(buffer);
    page.init(3);
    page.insert(record(10, 0x01));
    page.insert(record(10, 0x02));
    page.remove(0);

    auto error = errorFrom([&] { page.remove(0); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message().find("(3, 0)") != std::string::npos);

    error = errorFrom([&] { page.get(2); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message().find("(3, 2)") != std::string::npos);

    error = errorFrom([&] { page.remove(5); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);

    CHECK_THROWS_AS(page.slot(2), std::logic_error);
    CHECK_THROWS_AS(page.isLive(2), std::logic_error);
}

TEST_CASE("after a remove the next insert gets a new slot, never the dead one")
{
    Page buffer{};
    SlottedPage page(buffer);
    page.init(0);
    page.insert(record(13, 0x01));
    page.insert(record(20, 0x02));
    page.insert(record(13, 0x03));
    page.remove(1);
    CHECK(page.insert(record(5, 0x04)) == std::optional<std::uint16_t>(3));
    CHECK(page.slot(1) == SlotEntry{0, 0});
}

TEST_CASE("4084 bytes fit on an empty page, 4085 and more do not (D5)")
{
    Page buffer{};
    SlottedPage page(buffer);

    page.init(0);
    CHECK(page.insert(record(4084, 0x55)) == std::optional<std::uint16_t>(0));
    CHECK(page.freeSpace() == 0);
    CHECK(page.slot(0) == SlotEntry{12, 4084});

    page.init(0);
    CHECK_FALSE(page.insert(record(4085, 0x55)).has_value());
    page.init(0);
    CHECK_FALSE(page.insert(record(kMaxRecordLength, 0x55)).has_value());
    // Larger than a uint16 length can express: still just "no space".
    page.init(0);
    CHECK_FALSE(page.insert(record(70000, 0x55)).has_value());
    // A failed insert changes nothing.
    CHECK(page.slotCount() == 0);
    CHECK(page.freeEnd() == 4096);
}

TEST_CASE("a view must cover exactly one page")
{
    std::array<std::byte, kPageSize - 1> small{};
    CHECK_THROWS_AS(SlottedPage(std::span<std::byte>(small)), std::logic_error);
}

TEST_CASE("a header whose slot directory overlaps the record data is corrupt")
{
    Page buffer{};
    SlottedPage page(buffer);
    page.init(0);
    // 2000 slots would need 8 + 8000 bytes, past free_end = 4096.
    db::storage::writeU16(buffer, 4, 2000);
    CHECK_THROWS_AS(page.freeSpace(), std::runtime_error);
}
