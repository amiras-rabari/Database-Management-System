#include "loader/csv.h"

#include <utility>

#include "db/common/error.h"

namespace db::storage
{
    namespace
    {
        // Where the walk is inside the current field.
        enum class State
        {
            StartOfField, // nothing read for this field yet
            Unquoted,     // inside a field that did not start with "
            Quoted,       // inside "...": commas are text
            AfterQuote    // just read a " inside a quoted field: either the
                          // closing quote, or the first half of ""
        };
    } // namespace

    std::vector<std::string> splitCsvLine(std::string_view line)
    {
        // Windows line endings: getline leaves the \r of "\r\n" behind.
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        std::vector<std::string> fields;
        // A blank line has no fields at all (the loader reports the count).
        if (line.empty())
            return fields;

        std::string current;
        State state = State::StartOfField;
        // 1-based number of the field being read, for error messages.
        int column = 1;

        // One pass over the characters. A comma ends the field only when we
        // are not inside quotes.
        for (const char c : line)
        {
            switch (state)
            {
            case State::StartOfField:
                if (c == '"')
                    state = State::Quoted; // the opening quote is not data
                else if (c == ',')
                {
                    fields.push_back(std::move(current)); // an empty field
                    current.clear();
                    ++column;
                }
                else
                {
                    current += c;
                    state = State::Unquoted;
                }
                break;

            case State::Unquoted:
                if (c == ',')
                {
                    fields.push_back(std::move(current));
                    current.clear();
                    ++column;
                    state = State::StartOfField;
                }
                else
                    current += c; // a " here is plain text too
                break;

            case State::Quoted:
                if (c == '"')
                    state = State::AfterQuote; // closing quote, or half of ""
                else
                    current += c; // commas included
                break;

            case State::AfterQuote:
                if (c == '"')
                {
                    // "" inside quotes = one literal quote; still quoted.
                    current += '"';
                    state = State::Quoted;
                }
                else if (c == ',')
                {
                    fields.push_back(std::move(current));
                    current.clear();
                    ++column;
                    state = State::StartOfField;
                }
                else
                    throw Error(ErrorCategory::Syntax, "text after the closing quote", 0, column);
                break;
            }
        }

        if (state == State::Quoted)
            throw Error(ErrorCategory::Syntax, "unterminated quoted field", 0, column);

        // The last field has no comma after it.
        fields.push_back(std::move(current));
        return fields;
    }
} // namespace db::storage
