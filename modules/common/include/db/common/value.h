#pragma once

/// @file
/// Column types, the value held in one tuple field, value comparison and the
/// single rule for reading an integer from text.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace db
{

    /// Type of a column or value. No NULLs exist, so there is no null type.
    enum class Type
    {
        Int,
        Varchar
    };

    /// Name of a type as written in schemas and messages: `INT` or `VARCHAR`.
    std::string_view typeName(Type type);

    /// One field of a tuple: a 32-bit signed INT or a VARCHAR byte string.
    ///
    /// A Value does not know its column's VARCHAR length; parseValue checks it.
    class Value
    {
    public:
        /// Makes an INT value.
        static Value ofInt(std::int32_t value);
        /// Makes a VARCHAR value holding these bytes.
        static Value ofVarchar(std::string value);

        /// The type of this value.
        Type type() const;
        /// The INT held. Throws std::logic_error if this is not an INT.
        std::int32_t asInt() const;
        /// The VARCHAR bytes held. Throws std::logic_error if this is not a VARCHAR.
        const std::string& asVarchar() const;
        /// INT as canonical decimal (`-7`, `0`), VARCHAR as its raw bytes.
        std::string toText() const;

        /// Operator overloading: defining operator== means a == b works on Values.
        /// In C you'd write value_equals(a, b). In Java you'd override equals / implement compareTo
        /// True when both values have the same type and the same content.
        /// Values of different types are never equal. Exists so tuples can be
        /// stored in std::set / std::map; queries compare with compareValues.
        friend bool operator==(const Value& left, const Value& right);
        /// Strict total order: every INT before every VARCHAR, then by value
        /// (numeric for INT, byte-wise for VARCHAR). Exists only so tuples can
        /// be stored in std::set / std::map.
        friend bool operator<(const Value& left, const Value& right);

    private:
        //consider variant as a type-safe union in C
        //using Storage = ... is C's typedef, written in a more readable form.
        using Storage = std::variant<std::int32_t, std::string>;

        explicit Value(Storage data);

        Storage data_;
    };

    /// Compares two values of the same type: negative if `left` sorts first,
    /// 0 if equal, positive if `left` sorts last. INT compares numerically,
    /// VARCHAR byte-wise (each byte as unsigned, a prefix sorts first).
    /// Throws Error(Type) "Cannot compare INT with VARCHAR." when the types differ.
    int compareValues(const Value& left, const Value& right);

    /// Reads an INT: an optional `+` or `-`, then one or more decimal digits
    /// (leading zeros allowed), nothing else, within the int32 range.
    /// Returns std::nullopt for any other text. The one integer rule of the
    /// project: `7`, `007` and `+7` all give 7.
    std::optional<std::int32_t> parseInt32(std::string_view text);

} // namespace db
