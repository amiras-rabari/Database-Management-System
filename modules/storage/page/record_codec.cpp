#include "db/storage/record_codec.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "db/common/error.h"
#include "db/common/value.h"
#include "db/storage/bytes.h"

namespace db::storage
{

    namespace
    {

        // Field sizes of §M1.3: an INT is 4 bytes; a VARCHAR is a uint16
        // length prefix followed by its bytes.
        constexpr std::uint32_t kIntSize = 4;
        constexpr std::uint32_t kVarcharPrefixSize = 2;

        // Throws unless `tuple` can be stored under `schema`: one value per
        // column, each of its column's type, each VARCHAR at most n bytes.
        // Shared by encodedLength and encodeRecord so they reject the same
        // tuples.
        void checkTuple(const Tuple& tuple, const Schema& schema)
        {
            // First, so the loop below never indexes past the end of `tuple`.
            if (tuple.size() != schema.size())
            {
                throw Error(ErrorCategory::Schema, "Tuple has " + std::to_string(tuple.size())
                                                       + " values but the schema has "
                                                       + std::to_string(schema.size()) + " columns.");
            }

            for (std::size_t i = 0; i < schema.size(); ++i)
            {
                const Column& column = schema.column(i);
                const Value& value = tuple[i];

                if (value.type() != column.type)
                {
                    throw Error(ErrorCategory::Type, "Column '" + column.qualifiedName() + "' is "
                                                         + std::string(typeName(column.type)) + " but the value is "
                                                         + std::string(typeName(value.type())) + ".");
                }

                // L counts bytes, not characters (no UTF-8 check).
                // varcharLength is int, size() is size_t: converted
                // on purpose to avoid a signed/unsigned comparison.
                if (column.type == Type::Varchar
                    && value.asVarchar().size() > static_cast<std::size_t>(column.varcharLength))
                {
                    throw Error(ErrorCategory::Schema, "Value for column '" + column.qualifiedName() + "' is "
                                                           + std::to_string(value.asVarchar().size())
                                                           + " bytes, longer than VARCHAR("
                                                           + std::to_string(column.varcharLength) + ").");
                }
            }
        }

    } // namespace

    std::uint32_t encodedLength(const Tuple& tuple, const Schema& schema)
    {
        checkTuple(tuple, schema);

        // Summed per value, never derived from the schema alone: a VARCHAR's
        // length depends on the value (§M1.3 "must not assume fixed length").
        std::uint32_t length = 0;
        for (std::size_t i = 0; i < schema.size(); ++i)
        {
            if (schema.column(i).type == Type::Int)
                length += kIntSize;
            else
                // At most 255 bytes (checked above), so the cast loses nothing.
                length += kVarcharPrefixSize + static_cast<std::uint32_t>(tuple[i].asVarchar().size());
        }
        return length;
    }

    std::vector<std::byte> encodeRecord(const Tuple& tuple, const Schema& schema)
    {
        // Validates too, so a bad tuple throws before any bytes are made.
        const std::uint32_t length = encodedLength(tuple, schema);

        // Round brackets: `length` zero bytes, one allocation. (Braces would
        // make a vector holding the single value `length`.)
        std::vector<std::byte> bytes(length);
        std::size_t offset = 0;
        for (std::size_t i = 0; i < schema.size(); ++i)
        {
            if (schema.column(i).type == Type::Int)
            {
                writeI32(bytes, offset, tuple[i].asInt());
                offset += kIntSize;
            }
            else
            {
                const std::string& text = tuple[i].asVarchar();
                // At most 255 bytes (checked by encodedLength), so it fits a uint16.
                writeU16(bytes, offset, static_cast<std::uint16_t>(text.size()));
                offset += kVarcharPrefixSize;
                // The bytes as given: L counts bytes, no UTF-8 check.
                for (char c : text)
                    bytes[offset++] = static_cast<std::byte>(c);
            }
        }
        return bytes;
    }

    Tuple decodeRecord(std::span<const std::byte> bytes, const Schema& schema)
    {
        Tuple tuple;
        tuple.reserve(schema.size());
        std::size_t offset = 0;
        for (std::size_t i = 0; i < schema.size(); ++i)
        {
            // A cut-short INT or length prefix is caught by readI32/readU16,
            // which throw std::logic_error when fewer bytes remain.
            if (schema.column(i).type == Type::Int)
            {
                tuple.push_back(Value::ofInt(readI32(bytes, offset)));
                offset += kIntSize;
            }
            else
            {
                const std::uint16_t textLength = readU16(bytes, offset);
                offset += kVarcharPrefixSize;
                // offset <= bytes.size() here (readU16 checked), so the
                // subtraction cannot wrap.
                if (bytes.size() - offset < textLength)
                    throw std::logic_error("decodeRecord: column '" + schema.column(i).qualifiedName() + "' needs "
                                           + std::to_string(textLength) + " bytes but only "
                                           + std::to_string(bytes.size() - offset) + " remain");

                // Copied into a string the tuple owns: `bytes` usually points
                // into a buffer-pool frame that is reused after unpin, so a
                // view into it would later show another page's data.
                std::string text;
                text.reserve(textLength);
                for (std::size_t k = 0; k < textLength; ++k)
                    text += static_cast<char>(bytes[offset + k]);
                offset += textLength;
                tuple.push_back(Value::ofVarchar(std::move(text)));
            }
        }

        if (offset != bytes.size())
            throw std::logic_error("decodeRecord: " + std::to_string(bytes.size() - offset)
                                   + " byte(s) left after the last column");
        return tuple;
    }

} // namespace db::storage
