#include "db/storage/schema_spec.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/common/value.h"

namespace db::storage
{
    namespace
    {
        // `text` without spaces and tabs at either end.
        std::string_view trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            return text;
        }

        // ASCII upper case (type names are case-insensitive).
        std::string upper(std::string_view text)
        {
            std::string result(text);
            for (char& c : result)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return result;
        }

        bool isNameChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        // Turns one item, e.g. "b:VARCHAR(20)", into a bare column. `number`
        // is the item's 1-based position, for the messages.
        Column parseItem(std::string_view item, std::size_t number)
        {
            const std::string label = "Schema item " + std::to_string(number) + " '" + std::string(item) + "': ";
            auto fail = [&](const std::string& what) -> Error {
                return Error(ErrorCategory::Schema, label + what);
            };

            // name : type
            const std::size_t colon = item.find(':');
            if (colon == std::string_view::npos)
                throw fail("missing ':' between the column name and its type.");
            const std::string_view name = trim(item.substr(0, colon));
            const std::string_view type = trim(item.substr(colon + 1));

            if (name.empty())
                throw fail("the column name is empty.");
            for (const char c : name)
                if (!isNameChar(c))
                    throw fail("invalid column name (use only letters, digits and _).");

            const std::string typeUpper = upper(type);
            if (typeUpper == "INT")
                return Column{"", std::string(name), Type::Int, 0};

            // VARCHAR(n): check the prefix and the closing ')' explicitly.
            const std::string_view prefix = "VARCHAR(";
            if (typeUpper.size() > prefix.size() && typeUpper.compare(0, prefix.size(), prefix) == 0
                && typeUpper.back() == ')')
            {
                const std::string_view digits = type.substr(prefix.size(), type.size() - prefix.size() - 1);
                const std::optional<std::int32_t> n = parseInt32(digits);
                if (!n)
                    throw fail("VARCHAR length '" + std::string(digits) + "' is not a number.");
                if (*n < 1 || *n > kMaxVarcharLength)
                    throw fail("VARCHAR length " + std::to_string(*n) + " must be 1 to "
                        + std::to_string(kMaxVarcharLength) + ".");
                return Column{"", std::string(name), Type::Varchar, *n};
            }

            throw fail("unknown type '" + std::string(type) + "' (use INT or VARCHAR(n)).");
        }
    } // namespace

    Schema parseSchemaSpec(std::string_view text)
    {
        if (trim(text).empty())
            throw Error(ErrorCategory::Schema, "Schema text is empty (expected e.g. a:INT,b:VARCHAR(20)).");

        // Cut at each ',' with find; VARCHAR(n) never contains a comma.
        std::vector<Column> columns;
        std::size_t number = 1;
        while (true)
        {
            const std::size_t comma = text.find(',');
            const std::string_view item = text.substr(0, comma);
            if (trim(item).empty())
                throw Error(ErrorCategory::Schema, "Schema item " + std::to_string(number) + " is empty.");
            columns.push_back(parseItem(item, number));
            if (comma == std::string_view::npos)
                break;
            text.remove_prefix(comma + 1);
            ++number;
        }
        // The constructor rejects duplicate names ("Duplicate column 'a'.").
        return Schema(std::move(columns));
    }
} // namespace db::storage
