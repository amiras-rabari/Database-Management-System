// The SQL lexer (§M3.1). One pass over the text with a cursor; at each
// position the current character picks exactly one rule (see tokenizeSql).

#include <db/sql/lexer.h>

#include <db/common/error.h>
#include <db/common/value.h>
#include <db/sql/source_pos.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace db::sql
{

    namespace
    {

        // One entry of the keyword table: the upper-case spelling and its kind.
        struct Keyword
        {
            std::string_view spelling;
            SqlTokenKind kind;
        };

        // Every upper-case word of the §M3.2 grammar. All are reserved, so a
        // table or column can never be called `count` or `int`.
        // A plain array searched from the start: 33 entries, fixed order.
        constexpr Keyword kKeywords[] = {
            {"SELECT", SqlTokenKind::Select},     {"DISTINCT", SqlTokenKind::Distinct},
            {"FROM", SqlTokenKind::From},         {"WHERE", SqlTokenKind::Where},
            {"GROUP", SqlTokenKind::Group},       {"BY", SqlTokenKind::By},
            {"HAVING", SqlTokenKind::Having},     {"ORDER", SqlTokenKind::Order},
            {"ASC", SqlTokenKind::Asc},           {"DESC", SqlTokenKind::Desc},
            {"LIMIT", SqlTokenKind::Limit},       {"UNION", SqlTokenKind::Union},
            {"ALL", SqlTokenKind::All},           {"INTERSECT", SqlTokenKind::Intersect},
            {"EXCEPT", SqlTokenKind::Except},     {"JOIN", SqlTokenKind::Join},
            {"ON", SqlTokenKind::On},             {"AS", SqlTokenKind::As},
            {"AND", SqlTokenKind::And},           {"OR", SqlTokenKind::Or},
            {"NOT", SqlTokenKind::Not},           {"CREATE", SqlTokenKind::Create},
            {"TABLE", SqlTokenKind::Table},       {"INSERT", SqlTokenKind::Insert},
            {"INTO", SqlTokenKind::Into},         {"VALUES", SqlTokenKind::Values},
            {"INT", SqlTokenKind::Int},           {"VARCHAR", SqlTokenKind::Varchar},
            {"COUNT", SqlTokenKind::Count},       {"SUM", SqlTokenKind::Sum},
            {"AVG", SqlTokenKind::Avg},           {"MIN", SqlTokenKind::Min},
            {"MAX", SqlTokenKind::Max},
        };

        // Upper-cases 'a'..'z' only. std::toupper depends on the locale, and
        // the same SQL must give the same tokens on every machine.
        std::string asciiUpper(std::string_view word)
        {
            std::string upper(word);
            for (char& c : upper)
            {
                if (c >= 'a' && c <= 'z')
                {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            return upper;
        }

        // The keyword kind of a word, or Identifier when it is not a keyword.
        // `select`, `SELECT` and `SeLeCt` all upper-case to SELECT (§M3.1 row 1).
        SqlTokenKind classifyWord(std::string_view word)
        {
            const std::string upper = asciiUpper(word);
            for (const Keyword& keyword : kKeywords)
            {
                if (keyword.spelling == upper)
                {
                    return keyword.kind;
                }
            }
            return SqlTokenKind::Identifier;
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        // A character that may start an identifier (§M3.1 row 2).
        bool isWordStart(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        // A character that may continue an identifier.
        bool isWordPart(char c)
        {
            return isWordStart(c) || isDigit(c);
        }

        // Walks the text one character at a time and keeps the line and
        // column of the next character up to date.
        class Cursor
        {
        public:
            explicit Cursor(std::string_view text) : text_(text) {}

            bool atEnd() const
            {
                return index_ >= text_.size();
            }

            // The current character, or '\0' past the end.
            char peek() const
            {
                return index_ < text_.size() ? text_[index_] : '\0';
            }

            // The character after the current one, or '\0' past the end.
            char peekNext() const
            {
                return index_ + 1 < text_.size() ? text_[index_ + 1] : '\0';
            }

            // Steps over the current character. A '\n' starts a new line.
            // In "\r\n" the '\r' only moves the column and the '\n' then
            // resets it, so CRLF and LF text give the same positions.
            void advance()
            {
                if (text_[index_] == '\n')
                {
                    ++pos_.line;
                    pos_.column = 1;
                }
                else
                {
                    ++pos_.column;
                }
                ++index_;
            }

            std::size_t index() const
            {
                return index_;
            }

            SourcePos pos() const
            {
                return pos_;
            }

            // The text from `start` up to (not including) the current character.
            std::string_view textFrom(std::size_t start) const
            {
                return text_.substr(start, index_ - start);
            }

        private:
            std::string_view text_;
            std::size_t index_ = 0;
            SourcePos pos_;
        };

        Error lexicalError(const std::string& message, SourcePos pos)
        {
            return Error(ErrorCategory::Lexical, message, pos.line, pos.column);
        }

        // Reads an integer whose first character (digit or sign) is under the
        // cursor. The caller checked that a sign is followed by a digit.
        SqlToken readInteger(Cursor& cursor)
        {
            const SourcePos start = cursor.pos();
            const std::size_t startIndex = cursor.index();
            cursor.advance(); // the sign or the first digit
            while (isDigit(cursor.peek()))
            {
                cursor.advance();
            }
            const std::string_view digits = cursor.textFrom(startIndex);
            // "12abc" is not 12 followed by the word abc: it is a mistake.
            if (isWordStart(cursor.peek()))
            {
                throw lexicalError("number '" + std::string(digits) + "' runs straight into a letter", start);
            }
            // parseInt32 is the project's one integer rule (common).
            const std::optional<int32_t> value = parseInt32(digits);
            if (!value)
            {
                throw lexicalError("integer " + std::string(digits) + " is outside the INT range", start);
            }
            return SqlToken{SqlTokenKind::Integer, std::string(digits), start, *value};
        }

        // Reads a string whose opening quote is under the cursor. Two quotes
        // in a row inside the string add one quote to the value (§M3.1 row 3).
        SqlToken readString(Cursor& cursor)
        {
            const SourcePos start = cursor.pos();
            cursor.advance(); // the opening quote
            std::string value;
            while (true)
            {
                if (cursor.atEnd())
                {
                    // Reported at the opening quote: that is where the user
                    // must look to fix it.
                    throw lexicalError("unterminated string", start);
                }
                const char c = cursor.peek();
                if (c == '\'' && cursor.peekNext() == '\'')
                {
                    value += '\'';
                    cursor.advance();
                    cursor.advance();
                }
                else if (c == '\'')
                {
                    cursor.advance(); // the closing quote
                    return SqlToken{SqlTokenKind::String, value, start, 0};
                }
                else
                {
                    value += c;
                    cursor.advance();
                }
            }
        }

        // The kind of a one-character punctuation token, or nothing.
        std::optional<SqlTokenKind> punctuationKind(char c)
        {
            switch (c)
            {
            case '(':
                return SqlTokenKind::LeftParen;
            case ')':
                return SqlTokenKind::RightParen;
            case ',':
                return SqlTokenKind::Comma;
            case '.':
                return SqlTokenKind::Dot;
            case ';':
                return SqlTokenKind::Semicolon;
            case '*':
                return SqlTokenKind::Star;
            case '=':
                return SqlTokenKind::Equal;
            default:
                return std::nullopt;
            }
        }

    } // namespace

    std::vector<SqlToken> tokenizeSql(std::string_view text)
    {
        std::vector<SqlToken> tokens;
        Cursor cursor(text);
        while (!cursor.atEnd())
        {
            const char c = cursor.peek();
            const SourcePos start = cursor.pos();
            const std::size_t startIndex = cursor.index();

            // 1. Whitespace makes no token.
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                cursor.advance();
            }
            // 2. A comment runs to the end of the line. This test comes
            //    before the sign rule, so "--5" is a comment, not "-" "-5".
            else if (c == '-' && cursor.peekNext() == '-')
            {
                while (!cursor.atEnd() && cursor.peek() != '\n')
                {
                    cursor.advance();
                }
            }
            // 3. A word: keyword if its upper-case form is in the table,
            //    otherwise an identifier. Both keep the spelling as written.
            else if (isWordStart(c))
            {
                while (isWordPart(cursor.peek()))
                {
                    cursor.advance();
                }
                const std::string_view word = cursor.textFrom(startIndex);
                tokens.push_back(SqlToken{classifyWord(word), std::string(word), start, 0});
            }
            // 4. An integer. A sign counts only when a digit follows it
            //    directly: "-5" is one token, while "- 5" or a lone "-" is an
            //    error, since there is no arithmetic.
            else if (isDigit(c) || ((c == '+' || c == '-') && isDigit(cursor.peekNext())))
            {
                tokens.push_back(readInteger(cursor));
            }
            // 5. A string literal.
            else if (c == '\'')
            {
                tokens.push_back(readString(cursor));
            }
            // 6. '<' and '>' with maximal munch: always take the longest
            //    operator, so "<=" is one token and never "<" then "=".
            else if (c == '<' || c == '>')
            {
                const char next = cursor.peekNext();
                SqlTokenKind kind = c == '<' ? SqlTokenKind::Less : SqlTokenKind::Greater;
                cursor.advance();
                if (next == '=')
                {
                    kind = c == '<' ? SqlTokenKind::LessEqual : SqlTokenKind::GreaterEqual;
                    cursor.advance();
                }
                else if (c == '<' && next == '>')
                {
                    kind = SqlTokenKind::NotEqual;
                    cursor.advance();
                }
                tokens.push_back(SqlToken{kind, std::string(cursor.textFrom(startIndex)), start, 0});
            }
            // 7. One-character punctuation, and '='.
            else if (const std::optional<SqlTokenKind> kind = punctuationKind(c))
            {
                cursor.advance();
                tokens.push_back(SqlToken{*kind, std::string(1, c), start, 0});
            }
            // 8. Anything else: '#', '"', '!' (so "!=" too), a lone sign.
            else
            {
                throw lexicalError(std::string("unexpected character '") + c + "'", start);
            }
        }
        tokens.push_back(SqlToken{SqlTokenKind::EndOfInput, "", cursor.pos(), 0});
        return tokens;
    }

} // namespace db::sql
