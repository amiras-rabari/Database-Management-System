// Record codec: tuple <-> the exact bytes of SPEC §M1.3.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/record_codec.h"

namespace
{

    using db::Column;
    using db::ErrorCategory;
    using db::Schema;
    using db::Tuple;
    using db::Type;
    using db::Value;
    using db::storage::decodeRecord;
    using db::storage::encodedLength;
    using db::storage::encodeRecord;

    Column intColumn(const std::string& name)
    {
        return Column{"R", name, Type::Int, 0};
    }

    Column varcharColumn(const std::string& name, int n)
    {
        return Column{"R", name, Type::Varchar, n};
    }

    // Schema (a INT, b VARCHAR(n), c INT), the shape of the §M1.3 example.
    Schema exampleSchema(int n)
    {
        return Schema({intColumn("a"), varcharColumn("b", n), intColumn("c")});
    }

    // The bytes as upper-case hex pairs, e.g. "2A 00 00 00".
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

} // namespace

TEST_CASE("the SPEC M1.3 example encodes to exactly 13 bytes and decodes back")
{
    const Schema schema = exampleSchema(5);
    const Tuple tuple{Value::ofInt(42), Value::ofVarchar("bob"), Value::ofInt(7)};

    CHECK(encodedLength(tuple, schema) == 13);
    const std::vector<std::byte> bytes = encodeRecord(tuple, schema);
    CHECK(bytes.size() == 13);
    CHECK(hex(bytes) == "2A 00 00 00 03 00 62 6F 62 07 00 00 00");
    CHECK(decodeRecord(bytes, schema) == tuple);
}

TEST_CASE("the 20-byte record used by the page example")
{
    const Schema schema = exampleSchema(20);
    const Tuple tuple{Value::ofInt(1), Value::ofVarchar("abcdefghij"), Value::ofInt(2)};
    CHECK(encodedLength(tuple, schema) == 20);
    CHECK(encodeRecord(tuple, schema).size() == 20);
}

TEST_CASE("an all-INT schema has length 12 for any values")
{
    const Schema schema({intColumn("a"), intColumn("b"), intColumn("c")});
    const Tuple small{Value::ofInt(0), Value::ofInt(1), Value::ofInt(2)};
    const Tuple large{Value::ofInt(std::numeric_limits<std::int32_t>::max()), Value::ofInt(-5),
                      Value::ofInt(123456789)};
    CHECK(encodedLength(small, schema) == 12);
    CHECK(encodedLength(large, schema) == 12);
    CHECK(decodeRecord(encodeRecord(large, schema), schema) == large);
}

TEST_CASE("an empty VARCHAR encodes as a zero length")
{
    const Schema schema({varcharColumn("b", 5)});
    const Tuple tuple{Value::ofVarchar("")};
    CHECK(hex(encodeRecord(tuple, schema)) == "00 00");
    CHECK(encodedLength(tuple, schema) == 2);
    CHECK(decodeRecord(encodeRecord(tuple, schema), schema) == tuple);
}

TEST_CASE("VARCHAR of exactly n bytes is accepted, n + 1 is a schema error")
{
    const Schema schema({varcharColumn("b", 5)});
    const Tuple exact{Value::ofVarchar("abcde")};
    CHECK(encodedLength(exact, schema) == 7);
    CHECK(decodeRecord(encodeRecord(exact, schema), schema) == exact);

    const Tuple tooLong{Value::ofVarchar("abcdef")};
    auto error = errorFrom([&] { encodeRecord(tooLong, schema); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
    error = errorFrom([&] { encodedLength(tooLong, schema); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

TEST_CASE("a 2-byte UTF-8 character counts 2 towards the length")
{
    const Schema schema({varcharColumn("b", 2)});
    const Tuple tuple{Value::ofVarchar("\xC3\xA9")}; // 'é' in UTF-8
    CHECK(hex(encodeRecord(tuple, schema)) == "02 00 C3 A9");
    CHECK(decodeRecord(encodeRecord(tuple, schema), schema) == tuple);

    const Schema tooSmall({varcharColumn("b", 1)});
    CHECK(errorFrom([&] { encodeRecord(tuple, tooSmall); }).has_value());
}

TEST_CASE("negative and extreme INTs round-trip")
{
    const Schema schema({intColumn("a"), intColumn("b"), intColumn("c")});
    const Tuple tuple{Value::ofInt(-1), Value::ofInt(std::numeric_limits<std::int32_t>::min()),
                      Value::ofInt(std::numeric_limits<std::int32_t>::max())};
    const std::vector<std::byte> bytes = encodeRecord(tuple, schema);
    CHECK(hex(bytes) == "FF FF FF FF 00 00 00 80 FF FF FF 7F");
    CHECK(decodeRecord(bytes, schema) == tuple);
}

TEST_CASE("a tuple that does not fit the schema is rejected")
{
    const Schema schema = exampleSchema(5);

    const Tuple tooShort{Value::ofInt(42), Value::ofVarchar("bob")};
    auto error = errorFrom([&] { encodeRecord(tooShort, schema); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);

    const Tuple wrongType{Value::ofInt(42), Value::ofInt(1), Value::ofInt(7)};
    error = errorFrom([&] { encodeRecord(wrongType, schema); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Type);
    error = errorFrom([&] { encodedLength(wrongType, schema); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Type);
}

TEST_CASE("a truncated or overlong record is a logic_error")
{
    const Schema schema = exampleSchema(5);
    const Tuple tuple{Value::ofInt(42), Value::ofVarchar("bob"), Value::ofInt(7)};
    std::vector<std::byte> bytes = encodeRecord(tuple, schema);

    // Drop the last byte: the final INT is cut short.
    CHECK_THROWS_AS(decodeRecord(std::span(bytes).first(12), schema), std::logic_error);
    // Cut inside the VARCHAR: its length says 3 but only 1 byte follows.
    CHECK_THROWS_AS(decodeRecord(std::span(bytes).first(7), schema), std::logic_error);
    // One byte left over after the last column.
    bytes.push_back(std::byte{0});
    CHECK_THROWS_AS(decodeRecord(bytes, schema), std::logic_error);
}
