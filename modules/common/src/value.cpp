#include "db/common/value.h"

#include <limits>
#include <stdexcept>
#include <utility>

#include "db/common/error.h"

namespace db
{

    std::string_view typeName(Type type)
    {
        switch (type)
        {
            case Type::Int:
                return "INT";
            case Type::Varchar:
                return "VARCHAR";
        }
        throw std::logic_error("typeName: unknown Type value");
    }

    Value::Value(Storage data)
        : data_(std::move(data))
    {
    }

    Value Value::ofInt(std::int32_t value)
    {
        return Value(Storage(std::in_place_type<std::int32_t>, value));
    }

    Value Value::ofVarchar(std::string value)
    {
        return Value(Storage(std::in_place_type<std::string>, std::move(value)));
    }

    Type Value::type() const
    {
        return std::holds_alternative<std::int32_t>(data_) ? Type::Int : Type::Varchar;
    }

    std::int32_t Value::asInt() const
    {
        //The code uses get_if so that it can throw a logic_error with a clear message.
        //otherwise std::variant also throws a bad_variant_access exception if the type is not found.
        const std::int32_t* value = std::get_if<std::int32_t>(&data_);
        if (value == nullptr)
        {
            throw std::logic_error("Value::asInt called on a VARCHAR value");
        }
        return *value;
    }

    const std::string& Value::asVarchar() const
    {
        const std::string* value = std::get_if<std::string>(&data_);
        if (value == nullptr)
        {
            throw std::logic_error("Value::asVarchar called on an INT value");
        }
        return *value;
    }

    std::string Value::toText() const
    {
        if (type() == Type::Int)
        {
            return std::to_string(asInt());
        }
        return asVarchar();
    }

    // std::variant compares the alternative index first (INT is index 0), then
    // the held values; std::string's operator< is byte-wise on unsigned bytes.
    bool operator==(const Value& left, const Value& right)
    {
        return left.data_ == right.data_;
    }

    bool operator<(const Value& left, const Value& right)
    {
        return left.data_ < right.data_;
    }

    int compareValues(const Value& left, const Value& right)
    {
        if (left.type() != right.type())
        {
            throw Error(ErrorCategory::Type,
                        "Cannot compare " + std::string(typeName(left.type())) + " with " +
                            std::string(typeName(right.type())) + ".");
        }
        if (left.type() == Type::Int)
        {
            const std::int32_t a = left.asInt();
            const std::int32_t b = right.asInt();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        // char_traits<char>::compare orders bytes as unsigned char, so a byte
        // >= 0x80 sorts after every ASCII byte.
        const int result = left.asVarchar().compare(right.asVarchar());
        return result < 0 ? -1 : (result > 0 ? 1 : 0);
    }


    // The input is text of any length (e.g. "99999999999" in a data file),
    // so the number being built can grow past what 32 bits hold before we
    // know whether it fits. The work is therefore done in 64 bits:
    //  - magnitude is built digit by digit and checked against the limit
    //    after every digit, so "too big" is detected on the true value
    //    (in 32 bits the multiplication itself would overflow, which is
    //    undefined behaviour, and the check could not be trusted);
    //  - the sign is kept apart in `negative`, so magnitude is never
    //    negative;
    //  - the limit is 2147483647 for positive input and 2147483648 for
    //    negative input, because INT32_MIN's magnitude is one more than
    //    INT32_MAX (computed in 64 bits, since -INT32_MIN overflows 32 bits).
    // Leading zeros are allowed, so the digit count alone proves nothing:
    // "0000000000005" is 5. Only values that passed the check reach the
    // final narrowing to int32, so that cast never loses information.
    std::optional<std::int32_t> parseInt32(std::string_view text)
    {
        std::size_t position = 0;
        bool negative = false;
        if (position < text.size() && (text[position] == '+' || text[position] == '-'))
        {
            negative = text[position] == '-';
            ++position;
        }
        if (position == text.size())
        {
            return std::nullopt;
        }

        // The magnitude of INT32_MIN is one more than INT32_MAX, so accumulate
        // in 64 bits and stop as soon as it exceeds that; leading zeros make
        // the digit count unbounded, so the length alone proves nothing.
        const std::int64_t limit = negative
                                       ? -static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())
                                       : static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
        std::int64_t magnitude = 0;
        for (; position < text.size(); ++position)
        {
            const char c = text[position];
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
            magnitude = magnitude * 10 + (c - '0');
            if (magnitude > limit)
            {
                return std::nullopt;
            }
        }
        return static_cast<std::int32_t>(negative ? -magnitude : magnitude);
    }

} // namespace db
