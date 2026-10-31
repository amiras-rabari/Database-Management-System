// Schema text of `storage create-table --schema` (chunk 14): the spec's
// example, case-insensitive type names, spaces around parts, and every error.

#include <string>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/schema_spec.h"

namespace
{
    using namespace db;
    using db::storage::parseSchemaSpec;

    // The error parseSchemaSpec throws for `text` (fails the test if none).
    Error specError(const std::string& text)
    {
        try
        {
            parseSchemaSpec(text);
        }
        catch (const Error& e)
        {
            return e;
        }
        FAIL("no error for: " << text);
        return Error(ErrorCategory::Syntax, "unreachable");
    }

    bool contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }
} // namespace

TEST_CASE("schema spec: the spec's example gives three bare columns")
{
    const Schema s = parseSchemaSpec("a:INT,b:VARCHAR(20),c:INT");
    REQUIRE(s.size() == 3);
    CHECK(s.column(0).qualifier.empty());
    CHECK(s.column(0).name == "a");
    CHECK(s.column(0).type == Type::Int);
    CHECK(s.column(0).varcharLength == 0);
    CHECK(s.column(1).name == "b");
    CHECK(s.column(1).type == Type::Varchar);
    CHECK(s.column(1).varcharLength == 20);
    CHECK(s.column(2).name == "c");
    CHECK(s.column(2).type == Type::Int);
}

TEST_CASE("schema spec: type names are case-insensitive; names keep their case")
{
    const Schema s = parseSchemaSpec("Id:int,Name:Varchar(1),x_2:varchar(255)");
    REQUIRE(s.size() == 3);
    CHECK(s.column(0).name == "Id");
    CHECK(s.column(0).type == Type::Int);
    CHECK(s.column(1).varcharLength == 1);
    CHECK(s.column(2).name == "x_2");
    CHECK(s.column(2).varcharLength == 255);
}

TEST_CASE("schema spec: spaces around a name or a type are ignored")
{
    const Schema s = parseSchemaSpec(" a : INT , b:VARCHAR(5) ");
    REQUIRE(s.size() == 2);
    CHECK(s.column(0).name == "a");
    CHECK(s.column(1).name == "b");
    CHECK(s.column(1).varcharLength == 5);
}

TEST_CASE("schema spec: every error is Error(Schema) and names the item")
{
    struct Case
    {
        std::string text;
        std::string expected; // part of the message
    };
    const Case cases[] = {
        {"a", "Schema item 1 'a': missing ':'"},
        {"a:FLOAT", "Schema item 1 'a:FLOAT': unknown type 'FLOAT'"},
        {"a:INT,b:VARCHAR(0)", "Schema item 2 'b:VARCHAR(0)': VARCHAR length 0 must be 1 to 255"},
        {"a:VARCHAR(256)", "VARCHAR length 256 must be 1 to 255"},
        {"a:VARCHAR(-3)", "VARCHAR length -3 must be 1 to 255"},
        {"a:VARCHAR(x)", "VARCHAR length 'x' is not a number"},
        {"a:VARCHAR()", "VARCHAR length '' is not a number"},
        {"a:VARCHAR(20", "unknown type 'VARCHAR(20'"},
        {"a:VARCHAR", "unknown type 'VARCHAR'"},
        {"a:", "unknown type ''"},
        {":INT", "Schema item 1 ':INT': the column name is empty"},
        {"a b:INT", "invalid column name"},
        {"a:INT,,b:INT", "Schema item 2 is empty"},
        {"a:INT,", "Schema item 2 is empty"},
        {"", "Schema text is empty"},
        {"  ", "Schema text is empty"},
        {"a:INT,a:INT", "Duplicate column 'a'"},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.text);
        const Error e = specError(c.text);
        CHECK(e.category() == ErrorCategory::Schema);
        CHECK(contains(e.message(), c.expected));
    }
}
