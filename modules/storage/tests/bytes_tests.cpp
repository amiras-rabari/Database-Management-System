// Little-endian byte helpers: exact bytes from SPEC §M1.3's record example.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

#include <doctest/doctest.h>

#include "db/storage/bytes.h"
#include "db/storage/page.h"

namespace
{

    using namespace db::storage;

    // The bytes as upper-case hex pairs, e.g. "2A 00 00 00", so a failing
    // check prints something readable.
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

} // namespace

TEST_CASE("writeI32 gives the record-example bytes of SPEC M1.3")
{
    std::array<std::byte, 4> buffer{};
    writeI32(buffer, 0, 42);
    CHECK(hex(buffer) == "2A 00 00 00");
    writeI32(buffer, 0, 7);
    CHECK(hex(buffer) == "07 00 00 00");
}

TEST_CASE("writeU16 puts the low byte first")
{
    std::array<std::byte, 2> buffer{};
    writeU16(buffer, 0, 3);
    CHECK(hex(buffer) == "03 00");
    writeU16(buffer, 0, 0x1234);
    CHECK(hex(buffer) == "34 12");
    CHECK(readU16(buffer, 0) == 0x1234);
}

TEST_CASE("negative INT is two's complement and reads back unchanged")
{
    std::array<std::byte, 4> buffer{};
    writeI32(buffer, 0, -1);
    CHECK(hex(buffer) == "FF FF FF FF");
    CHECK(readI32(buffer, 0) == -1);

    const std::int32_t minimum = std::numeric_limits<std::int32_t>::min();
    writeI32(buffer, 0, minimum);
    CHECK(hex(buffer) == "00 00 00 80");
    CHECK(readI32(buffer, 0) == minimum);
}

TEST_CASE("writeU32 of kInvalidPageId and round trip at an inner offset")
{
    std::array<std::byte, 8> buffer{};
    writeU32(buffer, 0, kInvalidPageId);
    CHECK(hex(std::span(buffer).first(4)) == "FF FF FF FF");
    CHECK(readU32(buffer, 0) == kInvalidPageId);

    writeU32(buffer, 4, 0x01020304);
    CHECK(hex(std::span(buffer).last(4)) == "04 03 02 01");
    CHECK(readU32(buffer, 4) == 0x01020304);
}

TEST_CASE("an offset past the end of the view is a logic_error")
{
    std::array<std::byte, 4> buffer{};
    CHECK_THROWS_AS(readU16(buffer, 3), std::logic_error);
    CHECK_THROWS_AS(readU32(buffer, 1), std::logic_error);
    CHECK_THROWS_AS(readI32(buffer, 4), std::logic_error);
    CHECK_THROWS_AS(writeU16(buffer, 3, 1), std::logic_error);
    CHECK_THROWS_AS(writeU32(buffer, 1, 1), std::logic_error);
    CHECK_THROWS_AS(writeI32(buffer, 5, 1), std::logic_error);
    // An offset so large that offset + size wraps around must not pass.
    CHECK_THROWS_AS(readU32(buffer, std::numeric_limits<std::size_t>::max()), std::logic_error);

    // The last position that fits is accepted.
    writeU16(buffer, 2, 0xABCD);
    CHECK(readU16(buffer, 2) == 0xABCD);
}
