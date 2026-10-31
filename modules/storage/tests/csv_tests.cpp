// CSV line splitter (chunk 13): plain and quoted fields, "" inside quotes,
// commas inside quotes, spaces kept, one trailing \r removed, empty line =
// no fields, and the field number of each splitting error.

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "loader/csv.h"

namespace
{
    using db::Error;
    using db::ErrorCategory;
    using db::storage::splitCsvLine;
    using Fields = std::vector<std::string>;

    // The error splitCsvLine throws for `line` (fails the test if none).
    Error splitError(const std::string& line)
    {
        try
        {
            splitCsvLine(line);
        }
        catch (const Error& e)
        {
            return e;
        }
        FAIL("no error for: " << line);
        return Error(ErrorCategory::Syntax, "unreachable");
    }
} // namespace

TEST_CASE("csv: plain fields split at commas")
{
    CHECK(splitCsvLine("42,bob,7") == Fields{"42", "bob", "7"});
    CHECK(splitCsvLine("x") == Fields{"x"});
}

TEST_CASE("csv: empty fields, including leading and trailing ones")
{
    CHECK(splitCsvLine(",") == Fields{"", ""});
    CHECK(splitCsvLine("1,,2") == Fields{"1", "", "2"});
    CHECK(splitCsvLine("1,2,") == Fields{"1", "2", ""});
    CHECK(splitCsvLine(",1") == Fields{"", "1"});
}

TEST_CASE("csv: an empty line has no fields, also after removing \\r")
{
    CHECK(splitCsvLine("").empty());
    CHECK(splitCsvLine("\r").empty());
}

TEST_CASE("csv: quoted fields lose their quotes and may hold commas")
{
    CHECK(splitCsvLine("1,\"a,b\",2") == Fields{"1", "a,b", "2"});
    CHECK(splitCsvLine("1,\"bob\",2") == Fields{"1", "bob", "2"});
    CHECK(splitCsvLine("\"\",x") == Fields{"", "x"});
    CHECK(splitCsvLine("\",,,\"") == Fields{",,,"});
}

TEST_CASE("csv: a doubled quote inside quotes is one quote")
{
    CHECK(splitCsvLine("\"a\"\"b\"") == Fields{"a\"b"});
    CHECK(splitCsvLine("\"\"\"\"") == Fields{"\""});
    CHECK(splitCsvLine("1,\"say \"\"hi\"\", ok\",2") == Fields{"1", "say \"hi\", ok", "2"});
}

TEST_CASE("csv: a quote inside an unquoted field is plain text")
{
    CHECK(splitCsvLine("ab\"c,d") == Fields{"ab\"c", "d"});
}

TEST_CASE("csv: spaces are data, never trimmed")
{
    CHECK(splitCsvLine(" 7 , bob ") == Fields{" 7 ", " bob "});
    CHECK(splitCsvLine("\" a \"") == Fields{" a "});
}

TEST_CASE("csv: exactly one trailing \\r is removed; others stay")
{
    CHECK(splitCsvLine("42,bob,7\r") == Fields{"42", "bob", "7"});
    CHECK(splitCsvLine("1,\"a,b\"\r") == Fields{"1", "a,b"});
    CHECK(splitCsvLine("a\r\r") == Fields{"a\r"});
    CHECK(splitCsvLine("a\rb") == Fields{"a\rb"});
}

TEST_CASE("csv: unterminated quote names its field")
{
    const Error e = splitError("1,\"bob,2");
    CHECK(e.category() == ErrorCategory::Syntax);
    CHECK(e.line() == 0);
    CHECK(e.column() == 2);
    CHECK(e.message().find("unterminated") != std::string::npos);

    CHECK(splitError("\"").column() == 1);
    CHECK(splitError("a,b,\"c\"\"").column() == 3); // "" is a quote, not a close
}

TEST_CASE("csv: text after a closing quote names its field")
{
    const Error e = splitError("1,2,\"ab\"c");
    CHECK(e.category() == ErrorCategory::Syntax);
    CHECK(e.column() == 3);
    CHECK(e.message().find("after the closing quote") != std::string::npos);

    CHECK(splitError("\"a\" ,b").column() == 1); // a space is text too
}
