// Tests for the SQL lexer (tokenizeSql), taken from SPEC.md §M3.1 (lexical
// rules) and §M3.6 (error format), plus CHUNK_01 section 4.

#include <db/common/error.h>
#include <db/sql/lexer.h>
#include <db/sql/source_pos.h>

#include <doctest/doctest.h>

#include <string>
#include <vector>

using db::Error;
using db::ErrorCategory;
using db::sql::SourcePos;
using db::sql::SqlToken;
using db::sql::SqlTokenKind;
using db::sql::tokenizeSql;

namespace
{

    // The kinds of all tokens, end-of-input included.
    std::vector<SqlTokenKind> kindsOf(const std::string& text)
    {
        std::vector<SqlTokenKind> kinds;
        for (const SqlToken& token : tokenizeSql(text))
        {
            kinds.push_back(token.kind);
        }
        return kinds;
    }

    // Runs the lexer on text that must fail, and returns the lexical error.
    Error lexicalErrorOf(const std::string& text)
    {
        try
        {
            tokenizeSql(text);
        }
        catch (const Error& error)
        {
            CHECK(error.category() == ErrorCategory::Lexical);
            return error;
        }
        FAIL("expected a lexical error for: " << text);
        return Error(ErrorCategory::Lexical, "unreachable");
    }

    // Checks that text fails with a lexical error at line:column.
    void checkErrorAt(const std::string& text, int line, int column)
    {
        Error error = lexicalErrorOf(text);
        CHECK_MESSAGE(error.line() == line, text);
        CHECK_MESSAGE(error.column() == column, text);
    }

} // namespace

TEST_CASE("keywords are case-insensitive (§M3.1 row 1)")
{
    for (const std::string text : {"SELECT", "select", "SeLeCt"})
    {
        std::vector<SqlToken> tokens = tokenizeSql(text);
        REQUIRE(tokens.size() == 2);
        CHECK(tokens[0].kind == SqlTokenKind::Select);
        // The token keeps the spelling the user wrote.
        CHECK(tokens[0].text == text);
        CHECK(tokens[1].kind == SqlTokenKind::EndOfInput);
    }
}

TEST_CASE("identifiers are case-sensitive (§M3.1 row 2)")
{
    std::vector<SqlToken> tokens = tokenizeSql("Emp emp _x1");
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].kind == SqlTokenKind::Identifier);
    CHECK(tokens[1].kind == SqlTokenKind::Identifier);
    CHECK(tokens[2].kind == SqlTokenKind::Identifier);
    CHECK(tokens[0].text == "Emp");
    CHECK(tokens[1].text == "emp");
    CHECK(tokens[2].text == "_x1");
}

TEST_CASE("every grammar word is a reserved keyword")
{
    CHECK(kindsOf("count Varchar int max") ==
          std::vector<SqlTokenKind>{SqlTokenKind::Count, SqlTokenKind::Varchar, SqlTokenKind::Int,
                                    SqlTokenKind::Max, SqlTokenKind::EndOfInput});
    // A keyword inside a longer word is just part of an identifier.
    CHECK(kindsOf("selected") == std::vector<SqlTokenKind>{SqlTokenKind::Identifier, SqlTokenKind::EndOfInput});
}

TEST_CASE("two quotes inside a string mean one quote (§M3.1 row 3)")
{
    std::vector<SqlToken> tokens = tokenizeSql("'it''s' ''");
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].kind == SqlTokenKind::String);
    CHECK(tokens[0].text == "it's");
    CHECK(tokens[1].kind == SqlTokenKind::String);
    CHECK(tokens[1].text == "");
}

TEST_CASE("integers, optionally signed (§M3.1 row 4)")
{
    std::vector<SqlToken> tokens = tokenizeSql("-5 +7 007 2147483647 -2147483648");
    REQUIRE(tokens.size() == 6);
    for (int i = 0; i < 5; ++i)
    {
        CHECK(tokens[i].kind == SqlTokenKind::Integer);
    }
    CHECK(tokens[0].intValue == -5);
    CHECK(tokens[1].intValue == 7);
    CHECK(tokens[2].intValue == 7);
    CHECK(tokens[3].intValue == 2147483647);
    CHECK(tokens[4].intValue == -2147483647 - 1);
    // Out of int32 range: lexical error at the first character (DECISIONS 2026-10-18).
    checkErrorAt("x = 2147483648", 1, 5);
    checkErrorAt("x = -2147483649", 1, 5);
}

TEST_CASE("maximal munch for <=, >=, <> (§M3.1 row 5)")
{
    using K = SqlTokenKind;
    CHECK(kindsOf("a<=b") == std::vector<K>{K::Identifier, K::LessEqual, K::Identifier, K::EndOfInput});
    CHECK(kindsOf("a>=b") == std::vector<K>{K::Identifier, K::GreaterEqual, K::Identifier, K::EndOfInput});
    CHECK(kindsOf("a<>b") == std::vector<K>{K::Identifier, K::NotEqual, K::Identifier, K::EndOfInput});
    CHECK(kindsOf("a< =b") == std::vector<K>{K::Identifier, K::Less, K::Equal, K::Identifier, K::EndOfInput});
    CHECK(kindsOf("a<b>c") ==
          std::vector<K>{K::Identifier, K::Less, K::Identifier, K::Greater, K::Identifier, K::EndOfInput});
    CHECK(kindsOf("(),.;*") ==
          std::vector<K>{K::LeftParen, K::RightParen, K::Comma, K::Dot, K::Semicolon, K::Star, K::EndOfInput});
}

TEST_CASE("-- comments run to the end of the line (§M3.1 row 6)")
{
    std::vector<SqlToken> tokens = tokenizeSql("SELECT a -- comment FROM x\nFROM R");
    REQUIRE(tokens.size() == 5);
    CHECK(tokens[2].kind == SqlTokenKind::From);
    CHECK(tokens[2].pos == SourcePos{2, 1});
    // A comment on the last line, with no newline after it.
    CHECK(kindsOf("a --x") == std::vector<SqlTokenKind>{SqlTokenKind::Identifier, SqlTokenKind::EndOfInput});
    // "--5" is a comment, not minus minus five.
    CHECK(kindsOf("--5") == std::vector<SqlTokenKind>{SqlTokenKind::EndOfInput});
}

TEST_CASE("statements are separated by semicolons (§M3.1 row 7)")
{
    using K = SqlTokenKind;
    CHECK(kindsOf("SELECT a FROM R; SELECT b FROM S;") ==
          std::vector<K>{K::Select, K::Identifier, K::From, K::Identifier, K::Semicolon, K::Select, K::Identifier,
                         K::From, K::Identifier, K::Semicolon, K::EndOfInput});
    CHECK(kindsOf("") == std::vector<K>{K::EndOfInput});
}

TEST_CASE("positions are 1-based and CRLF counts as one newline")
{
    std::vector<SqlToken> lf = tokenizeSql("SELECT\n  name");
    REQUIRE(lf.size() == 3);
    CHECK(lf[0].pos == SourcePos{1, 1});
    CHECK(lf[1].pos == SourcePos{2, 3});
    CHECK(lf[2].pos == SourcePos{2, 7}); // end of input, just after "name"

    std::vector<SqlToken> crlf = tokenizeSql("SELECT\r\n  name");
    REQUIRE(crlf.size() == 3);
    CHECK(crlf[1].pos == SourcePos{2, 3});
    CHECK(crlf[2].pos == SourcePos{2, 7});

    // A string's position is its opening quote, also across a newline in it.
    std::vector<SqlToken> str = tokenizeSql("x 'a\nb' y");
    REQUIRE(str.size() == 4);
    CHECK(str[1].pos == SourcePos{1, 3});
    CHECK(str[1].text == "a\nb");
    CHECK(str[2].pos == SourcePos{2, 4});
}

TEST_CASE("lexical errors at the exact position")
{
    checkErrorAt("SELECT 'abc", 1, 8);  // unterminated string: at the opening quote
    checkErrorAt("a\n  #", 2, 3);       // unexpected character
    checkErrorAt("a - b", 1, 3);        // lone minus: there is no arithmetic
    checkErrorAt("x = - 5", 1, 5);      // the sign must touch its digits
    checkErrorAt("+", 1, 1);
    checkErrorAt("x = 12abc", 1, 5);    // digits run straight into a letter
    checkErrorAt("SELECT \"Emp\"", 1, 8); // double quotes are not SQL strings here
    checkErrorAt("a != 1", 1, 3);       // only <> means "not equal"
}

TEST_CASE("formatError gives the §M3.6 text")
{
    Error error = lexicalErrorOf("SELECT 'abc");
    CHECK(db::formatError(error) == "error: lexical: unterminated string at line 1, column 8");
    Error hash = lexicalErrorOf("a #");
    CHECK(db::formatError(hash) == "error: lexical: unexpected character '#' at line 1, column 3");
}
