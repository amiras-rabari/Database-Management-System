// The library tokenizer (db::tokenize).

#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/token.h"
#include "db/common/tokenizer.h"

namespace
{

    using db::ErrorCategory;
    using db::Token;
    using db::TokenKind;

    std::vector<TokenKind> kinds(const std::string& text)
    {
        std::vector<TokenKind> result;
        for (const Token& token : db::tokenize(text))
        {
            result.push_back(token.kind);
        }
        return result;
    }

    // The db::Error that tokenizing `text` throws, or nullopt if it throws none.
    std::optional<db::Error> lexError(const std::string& text)
    {
        try
        {
            db::tokenize(text);
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    void checkLexError(const std::string& text, int line, int column)
    {
        INFO("input: ", text);
        const std::optional<db::Error> error = lexError(text);
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Lexical);
        CHECK(error->line() == line);
        CHECK(error->column() == column);
    }

} // namespace

TEST_CASE("tokenize gives every RA keyword in lowercase only")
{
    CHECK(kinds("select project rename join union intersect minus times distinct") ==
          std::vector<TokenKind>{TokenKind::Select, TokenKind::Project, TokenKind::Rename, TokenKind::Join,
                                 TokenKind::Union, TokenKind::Intersect, TokenKind::Minus, TokenKind::Times,
                                 TokenKind::Distinct, TokenKind::End});
    CHECK(kinds("SELECT Union") ==
          std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Identifier, TokenKind::End});
}

TEST_CASE("tokenize gives and/or/not in any case")
{
    CHECK(kinds("and AND And or OR Or not NOT nOt") ==
          std::vector<TokenKind>{TokenKind::And, TokenKind::And, TokenKind::And, TokenKind::Or, TokenKind::Or,
                                 TokenKind::Or, TokenKind::Not, TokenKind::Not, TokenKind::Not, TokenKind::End});
    CHECK(kinds("android order nothing") ==
          std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Identifier, TokenKind::Identifier,
                                 TokenKind::End});
}

TEST_CASE("tokenize gives operators and punctuation; <> and != are both NotEqual")
{
    CHECK(kinds("= <> != < <= > >= ( ) [ ] , .") ==
          std::vector<TokenKind>{TokenKind::Equal, TokenKind::NotEqual, TokenKind::NotEqual, TokenKind::Less,
                                 TokenKind::LessEqual, TokenKind::Greater, TokenKind::GreaterEqual,
                                 TokenKind::LeftParen, TokenKind::RightParen, TokenKind::LeftBracket,
                                 TokenKind::RightBracket, TokenKind::Comma, TokenKind::Dot, TokenKind::End});
}

TEST_CASE("tokenize keeps the lexeme of identifiers, integers and strings")
{
    const std::vector<Token> tokens = db::tokenize("Emp_1 007 'a b'");
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].kind == TokenKind::Identifier);
    CHECK(tokens[0].text == "Emp_1");
    CHECK(tokens[1].kind == TokenKind::Integer);
    CHECK(tokens[1].text == "007");
    CHECK(tokens[1].intValue == 7);
    CHECK(tokens[2].kind == TokenKind::String);
    CHECK(tokens[2].text == "'a b'");
    CHECK(tokens[2].stringValue == "a b");
    CHECK(tokens[3].kind == TokenKind::End);
    CHECK(tokens[3].text.empty());
}

TEST_CASE("a sign directly before a digit belongs to the integer (Bonus 1 case 4)")
{
    const std::vector<Token> tokens = db::tokenize("Age>-30");
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].text == "Age");
    CHECK(tokens[1].kind == TokenKind::Greater);
    CHECK(tokens[2].kind == TokenKind::Integer);
    CHECK(tokens[2].text == "-30");
    CHECK(tokens[2].intValue == -30);
    CHECK(db::tokenize("+7")[0].intValue == 7);
    CHECK(db::tokenize("-2147483648")[0].intValue == -2147483647 - 1);
}

TEST_CASE("a doubled quote inside a string is one quote (Bonus 1 case 7)")
{
    const std::vector<Token> tokens = db::tokenize("'O''Brien'");
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].kind == TokenKind::String);
    CHECK(tokens[0].stringValue == "O'Brien");
    CHECK(tokens[0].text == "'O''Brien'");
    CHECK(db::tokenize("''")[0].stringValue.empty());
    CHECK(db::tokenize("''''")[0].stringValue == "'");
    CHECK(db::tokenize("'Bob)'")[0].stringValue == "Bob)");
}

TEST_CASE("an RA keyword inside a condition is still its keyword token (Bonus 1 case 8)")
{
    CHECK(kinds("select[union=3](R)") ==
          std::vector<TokenKind>{TokenKind::Select, TokenKind::LeftBracket, TokenKind::Union, TokenKind::Equal,
                                 TokenKind::Integer, TokenKind::RightBracket, TokenKind::LeftParen,
                                 TokenKind::Identifier, TokenKind::RightParen, TokenKind::End});
}

TEST_CASE("tokens carry 1-based line and column")
{
    const std::vector<Token> tokens = db::tokenize("select[a=1]\n  (R)");
    REQUIRE(tokens.size() == 10);
    CHECK(tokens[0].line == 1);
    CHECK(tokens[0].column == 1);
    CHECK(tokens[2].text == "a");
    CHECK(tokens[2].column == 8);
    CHECK(tokens[6].kind == TokenKind::LeftParen);
    CHECK(tokens[6].line == 2);
    CHECK(tokens[6].column == 3);
    CHECK(tokens[7].line == 2);
    CHECK(tokens[7].column == 4);
    CHECK(tokens[9].kind == TokenKind::End);
    CHECK(tokens[9].line == 2);
    CHECK(tokens[9].column == 6);
}

TEST_CASE("every whitespace character separates tokens, including \\v \\f \\r \\t")
{
    CHECK(kinds("a\v=\f1\r\n\tb") ==
          std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Equal, TokenKind::Integer,
                                 TokenKind::Identifier, TokenKind::End});
    CHECK(kinds(" \v\f\t\r\n ") == std::vector<TokenKind>{TokenKind::End});
}

TEST_CASE("// starts a comment to the end of the line")
{
    CHECK(kinds("a // = 1 'unterminated\n= 1") ==
          std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Equal, TokenKind::Integer, TokenKind::End});
    CHECK(kinds("// only a comment") == std::vector<TokenKind>{TokenKind::End});
}

TEST_CASE("an unterminated string is a lexical error at the opening quote (Bonus 1 case 9)")
{
    checkLexError("select[Name='Bob](R)", 1, 13);
    checkLexError("a = 'x\n'", 1, 5);
}

TEST_CASE("a lone sign is a lexical error")
{
    checkLexError("a = -", 1, 5);
    checkLexError("a = - 1", 1, 5);
    checkLexError("a = +b", 1, 5);
}

TEST_CASE("! not followed by = is a lexical error")
{
    checkLexError("a ! b", 1, 3);
}

TEST_CASE("a word that starts with a digit is a lexical error")
{
    checkLexError("x = 123abc", 1, 5);
    checkLexError("x = 1_", 1, 5);
    checkLexError("x = -1abc", 1, 5);
}

TEST_CASE("an integer outside the INT range is a lexical error")
{
    checkLexError("a = 2147483648", 1, 5);
    checkLexError("a = -2147483649", 1, 5);
    const std::optional<db::Error> error = lexError("2147483648");
    REQUIRE(error.has_value());
    CHECK(error->message() == "Integer literal out of INT range: 2147483648.");
}

TEST_CASE("characters outside the language are lexical errors, including braces")
{
    checkLexError("a # b", 1, 3);
    checkLexError("{ 1 }", 1, 1);
    checkLexError("a / b", 1, 3);
    checkLexError("a\n  \xC3\xA9", 2, 3);
}
