#include "db/common/tokenizer.h"

#include <cstddef>
#include <optional>
#include <string>

#include "db/common/error.h"
#include "db/common/value.h"

namespace db
{

    namespace
    {

        // ASCII only, so the result never depends on the global C locale.
        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool isLetter(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        bool isWordCharacter(char c)
        {
            return isLetter(c) || isDigit(c) || c == '_';
        }

        // The characters std::isspace accepts in the "C" locale.
        bool isWhitespace(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        }

        struct Keyword
        {
            //look at learning notes on why we choose to use std::string_view instead of std::string here
            std::string_view word;
            TokenKind kind;
        };

        //constexpr Keyword kRaKeywords[] declares a compile-time constant array containing instances
        //of a struct/class named Keyword
        constexpr Keyword kRaKeywords[] = {
            {"select", TokenKind::Select},
            {"project", TokenKind::Project},
            {"rename", TokenKind::Rename},
            {"join", TokenKind::Join},
            {"union", TokenKind::Union},
            {"intersect", TokenKind::Intersect},
            {"minus", TokenKind::Minus},
            {"times", TokenKind::Times},
            {"distinct", TokenKind::Distinct},
        };

        constexpr Keyword kConditionKeywords[] = {
            {"and", TokenKind::And},
            {"or", TokenKind::Or},
            {"not", TokenKind::Not},
        };

        // The kind of a word: an RA keyword only in its exact lowercase
        // spelling, a condition keyword in any case, else an identifier.
        TokenKind wordKind(std::string_view word)
        {
            for (const Keyword& keyword : kRaKeywords)
            {
                if (word == keyword.word)
                {
                    return keyword.kind;
                }
            }
            std::string lower(word);
            for (char& c : lower)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            for (const Keyword& keyword : kConditionKeywords)
            {
                if (lower == keyword.word)
                {
                    return keyword.kind;
                }
            }
            return TokenKind::Identifier;
        }

        // Scans the input left to right; each scan function consumes one
        // token, so the main loop always makes progress.
        class Scanner
        {
        public:
            //The explicit keyword disables this auto-conversion.
            //It forces callers to instantiate the object intentionally.Just like we do in Java,
            //this prevents implicit conversions from std::string to Scanner.
            //Rule of Thumb: Mark all single-argument constructors as explicit by default.
            //Exceptions: Only omit explicit when you intentionally want implicit conversion
            //(e.g., a custom String class taking a const char*).
            explicit Scanner(std::string_view text)
                : text_(text)
            {
            }

            std::vector<Token> run()
            {
                skipWhitespaceAndComments();
                while (!atEnd())
                {
                    scanToken();
                    skipWhitespaceAndComments();
                }
                Token end;
                end.kind = TokenKind::End;
                end.line = line_;
                end.column = column_;
                tokens_.push_back(end);
                return std::move(tokens_);
            }

        private:
            bool atEnd() const
            {
                return position_ >= text_.size();
            }

            char peek(std::size_t ahead = 0) const
            {
                return position_ + ahead < text_.size() ? text_[position_ + ahead] : '\0';
            }

            void advance()
            {
                if (text_[position_] == '\n')
                {
                    ++line_;
                    column_ = 1;
                }
                else
                {
                    ++column_;
                }
                ++position_;
            }

            [[noreturn]] void fail(const std::string& message) const
            {
                throw Error(ErrorCategory::Lexical, message, startLine_, startColumn_);
            }

            void skipWhitespaceAndComments()
            {
                while (!atEnd())
                {
                    if (isWhitespace(peek()))
                    {
                        advance();
                    }
                    else if (peek() == '/' && peek(1) == '/')
                    {
                        while (!atEnd() && peek() != '\n')
                        {
                            advance();
                        }
                    }
                    else
                    {
                        return;
                    }
                }
            }

            void addToken(TokenKind kind)
            {
                Token token;
                token.kind = kind;
                token.text = std::string(text_.substr(start_, position_ - start_));
                token.line = startLine_;
                token.column = startColumn_;
                tokens_.push_back(std::move(token));
            }

            void scanToken()
            {
                start_ = position_;
                startLine_ = line_;
                startColumn_ = column_;
                const char c = peek();
                advance();

                switch (c)
                {
                    case '(':
                        addToken(TokenKind::LeftParen);
                        return;
                    case ')':
                        addToken(TokenKind::RightParen);
                        return;
                    case '[':
                        addToken(TokenKind::LeftBracket);
                        return;
                    case ']':
                        addToken(TokenKind::RightBracket);
                        return;
                    case ',':
                        addToken(TokenKind::Comma);
                        return;
                    case '.':
                        addToken(TokenKind::Dot);
                        return;
                    case '=':
                        addToken(TokenKind::Equal);
                        return;
                    case '!':
                        if (peek() != '=')
                        {
                            fail("Expected '!=' but found '!'.");
                        }
                        advance();
                        addToken(TokenKind::NotEqual);
                        return;
                    case '<':
                        if (peek() == '=' || peek() == '>')
                        {
                            const TokenKind kind = peek() == '=' ? TokenKind::LessEqual : TokenKind::NotEqual;
                            advance();
                            addToken(kind);
                            return;
                        }
                        addToken(TokenKind::Less);
                        return;
                    case '>':
                        if (peek() == '=')
                        {
                            advance();
                            addToken(TokenKind::GreaterEqual);
                            return;
                        }
                        addToken(TokenKind::Greater);
                        return;
                    case '\'':
                        scanString();
                        return;
                    case '+':
                    case '-':
                        if (!isDigit(peek()))
                        {
                            fail(std::string("Standalone sign '") + c + "' is not allowed.");
                        }
                        scanInteger();
                        return;
                    default:
                        break;
                }

                if (isDigit(c))
                {
                    scanInteger();
                }
                else if (isLetter(c))
                {
                    scanWord();
                }
                else
                {
                    fail(std::string("Unexpected character '") + c + "'.");
                }
            }

            // Called after the opening quote.
            void scanString()
            {
                std::string value;
                while (true)
                {
                    if (atEnd())
                    {
                        fail("Unterminated string (reached end of input).");
                    }
                    if (peek() == '\n')
                    {
                        fail("Unterminated string (newline found before closing quote).");
                    }
                    if (peek() == '\'')
                    {
                        advance();
                        if (peek() != '\'')
                        {
                            break;
                        }
                    }
                    value += peek();
                    advance();
                }
                addToken(TokenKind::String);
                tokens_.back().stringValue = std::move(value);
            }

            // Called after the first character (a digit or a sign).
            void scanInteger()
            {
                while (isDigit(peek()))
                {
                    advance();
                }
                // Bonus 1 rejects a word that starts with a digit (123abc)
                // instead of splitting it into an integer and an identifier.
                if (isWordCharacter(peek()))
                {
                    while (isWordCharacter(peek()))
                    {
                        advance();
                    }
                    fail("Identifier must start with a letter.");
                }
                const std::string_view lexeme = text_.substr(start_, position_ - start_);
                const std::optional<std::int32_t> value = parseInt32(lexeme);
                if (!value)
                {
                    fail("Integer literal out of INT range: " + std::string(lexeme) + ".");
                }
                addToken(TokenKind::Integer);
                tokens_.back().intValue = *value;
            }

            // Called after the first letter.
            void scanWord()
            {
                while (isWordCharacter(peek()))
                {
                    advance();
                }
                addToken(wordKind(text_.substr(start_, position_ - start_)));
            }

            std::string_view text_;
            std::vector<Token> tokens_;
            std::size_t position_ = 0;
            int line_ = 1;
            int column_ = 1;
            std::size_t start_ = 0;
            int startLine_ = 1;
            int startColumn_ = 1;
        };

    } // namespace

    std::vector<Token> tokenize(std::string_view text)
    {
        return Scanner(text).run();
    }

} // namespace db
