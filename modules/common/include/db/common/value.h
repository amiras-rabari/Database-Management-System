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
    /// Real is only AVG's result (SPEC §M2.3): it appears in query results
    /// and temporary runs, never as a table column (DECISIONS 2026-11-07).
    enum class Type
    {
        Int,
        Varchar,
        Real
    };

    /// Name of a type as written in schemas and messages: `INT`, `VARCHAR`
    /// or `REAL`.
    std::string_view typeName(Type type);

    /// One field of a tuple: a 32-bit signed INT, a VARCHAR byte string, or
    /// a REAL (a double, the result of AVG).
    ///
    /// A Value does not know its column's VARCHAR length; parseValue checks it.
    class Value
    {
    public:
        /// Makes an INT value.
        static Value ofInt(std::int32_t value);
        /// Makes a VARCHAR value holding these bytes.
        static Value ofVarchar(std::string value);
        /// Makes a REAL value.
        static Value ofReal(double value);

        /// The type of this value.
        Type type() const;
        /// The INT held. Throws std::logic_error if this is not an INT.
        std::int32_t asInt() const;
        /// The VARCHAR bytes held. Throws std::logic_error if this is not a VARCHAR.
        const std::string& asVarchar() const;
        /// The REAL held. Throws std::logic_error if this is not a REAL.
        double asReal() const;
        /// INT as canonical decimal (`-7`, `0`), VARCHAR as its raw bytes,
        /// REAL with exactly four decimals (`3.5000`, `-0.3333`; SPEC §M2.3).
        std::string toText() const;

        /// Operator overloading: defining operator== means a == b works on Values.
        /// In C you'd write value_equals(a, b). In Java you'd override equals / implement compareTo
        /// True when both values have the same type and the same content.
        /// Values of different types are never equal. Exists so tuples can be
        /// stored in std::set / std::map; queries compare with compareValues.
        friend bool operator==(const Value& left, const Value& right);
        /// Strict total order: every INT before every VARCHAR before every
        /// REAL, then by value (numeric for INT and REAL, byte-wise for
        /// VARCHAR). Exists only so tuples can
        /// be stored in std::set / std::map.
        friend bool operator<(const Value& left, const Value& right);

    private:
        //consider variant as a type-safe union in C
        //using Storage = ... is C's typedef, written in a more readable form.
        using Storage = std::variant<std::int32_t, std::string, double>;

        explicit Value(Storage data);

        Storage data_;
    };

    /// True when values of these two types may be compared: the same type,
    /// or INT with REAL (both are numbers, e.g. `HAVING AVG(salary) > 50000`;
    /// DECISIONS 2026-11-07). VARCHAR compares only with VARCHAR.
    bool typesComparable(Type left, Type right);

    /// Compares two values: negative if `left` sorts first, 0 if equal,
    /// positive if `left` sorts last. INT and REAL compare as numbers (an INT
    /// becomes a double, which holds every int32 exactly). VARCHAR compares
    /// byte-wise (each byte as unsigned, a prefix sorts first).
    /// Throws Error(Type) "Cannot compare INT with VARCHAR." when
    /// typesComparable is false.
    int compareValues(const Value& left, const Value& right);

    /// Reads an INT: an optional `+` or `-`, then one or more decimal digits
    /// (leading zeros allowed), nothing else, within the int32 range.
    /// Returns std::nullopt for any other text. The one integer rule of the
    /// project: `7`, `007` and `+7` all give 7.
    std::optional<std::int32_t> parseInt32(std::string_view text);

} // namespace db
