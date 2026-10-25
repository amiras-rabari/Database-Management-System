// Core types: db::Error, db::Value, parseInt32, db::Schema and the shared
// schema rules.

#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace
{

    using db::Column;
    using db::ColumnRef;
    using db::ErrorCategory;
    using db::Schema;
    using db::Type;
    using db::Value;

    // Runs `function` and returns the db::Error it threw, or nullopt if it
    // threw nothing.
    template <typename Function>
    std::optional<db::Error> errorFrom(Function function)
    {
        try
        {
            function();
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    Column intColumn(const std::string& qualifier, const std::string& name)
    {
        return Column{qualifier, name, Type::Int, 0};
    }

    Column varcharColumn(const std::string& qualifier, const std::string& name, int length)
    {
        return Column{qualifier, name, Type::Varchar, length};
    }

    ColumnRef bare(const std::string& name)
    {
        return ColumnRef{std::nullopt, name};
    }

    ColumnRef qualified(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    // Emp(EID INT, Name VARCHAR(20), DID INT)
    Schema empSchema()
    {
        return Schema({intColumn("Emp", "EID"), varcharColumn("Emp", "Name", 20), intColumn("Emp", "DID")});
    }

    // Dept(DID INT, DName VARCHAR(20))
    Schema deptSchema()
    {
        return Schema({intColumn("Dept", "DID"), varcharColumn("Dept", "DName", 20)});
    }

    std::vector<std::string> qualifiedNames(const Schema& schema)
    {
        std::vector<std::string> names;
        for (const Column& column : schema.columns())
        {
            names.push_back(column.qualifiedName());
        }
        return names;
    }

} // namespace

// --- parseInt32 -------------------------------------------------------------

TEST_CASE("parseInt32 gives one value however the integer is written (I-2)")
{
    CHECK(db::parseInt32("7") == 7);
    CHECK(db::parseInt32("007") == 7);
    CHECK(db::parseInt32("+7") == 7);
    CHECK(db::parseInt32("00000000000000000007") == 7);
    CHECK(db::parseInt32("-0") == 0);
    CHECK(db::parseInt32("-12") == -12);
}

TEST_CASE("parseInt32 accepts exactly the int32 range")
{
    CHECK(db::parseInt32("2147483647") == 2147483647);
    CHECK(db::parseInt32("-2147483648") == -2147483647 - 1);
    CHECK_FALSE(db::parseInt32("2147483648").has_value());
    CHECK_FALSE(db::parseInt32("-2147483649").has_value());
    CHECK_FALSE(db::parseInt32("99999999999999999999").has_value());
}

TEST_CASE("parseInt32 rejects anything but sign and digits")
{
    CHECK_FALSE(db::parseInt32("").has_value());
    CHECK_FALSE(db::parseInt32("-").has_value());
    CHECK_FALSE(db::parseInt32("+").has_value());
    CHECK_FALSE(db::parseInt32("1a").has_value());
    CHECK_FALSE(db::parseInt32(" 1").has_value());
    CHECK_FALSE(db::parseInt32("1 ").has_value());
    CHECK_FALSE(db::parseInt32("+-1").has_value());
    CHECK_FALSE(db::parseInt32("1.0").has_value());
}

// --- Value ------------------------------------------------------------------

TEST_CASE("Value holds an INT or a VARCHAR and reports its type")
{
    const Value number = Value::ofInt(-42);
    const Value text = Value::ofVarchar("Alice");

    CHECK(number.type() == Type::Int);
    CHECK(number.asInt() == -42);
    CHECK(text.type() == Type::Varchar);
    CHECK(text.asVarchar() == "Alice");
    CHECK(db::typeName(Type::Int) == "INT");
    CHECK(db::typeName(Type::Varchar) == "VARCHAR");
}

TEST_CASE("Value access with the wrong type is an internal error")
{
    CHECK_THROWS_AS(Value::ofVarchar("7").asInt(), std::logic_error);
    CHECK_THROWS_AS(Value::ofInt(7).asVarchar(), std::logic_error);
}

TEST_CASE("Value toText gives canonical decimal for INT and raw bytes for VARCHAR")
{
    CHECK(Value::ofInt(7).toText() == "7");
    CHECK(Value::ofInt(-2147483647 - 1).toText() == "-2147483648");
    CHECK(Value::ofInt(*db::parseInt32("+007")).toText() == "7");
    CHECK(Value::ofVarchar("it's").toText() == "it's");
    CHECK(Value::ofVarchar("").toText() == "");
}

TEST_CASE("compareValues orders INT numerically")
{
    CHECK(db::compareValues(Value::ofInt(-5), Value::ofInt(3)) < 0);
    CHECK(db::compareValues(Value::ofInt(3), Value::ofInt(-5)) > 0);
    CHECK(db::compareValues(Value::ofInt(-1), Value::ofInt(-10)) > 0);
    CHECK(db::compareValues(Value::ofInt(7), Value::ofInt(*db::parseInt32("007"))) == 0);
}

TEST_CASE("compareValues orders VARCHAR byte-wise")
{
    CHECK(db::compareValues(Value::ofVarchar("B"), Value::ofVarchar("a")) < 0);
    CHECK(db::compareValues(Value::ofVarchar("a"), Value::ofVarchar("ab")) < 0);
    CHECK(db::compareValues(Value::ofVarchar(""), Value::ofVarchar("a")) < 0);
    CHECK(db::compareValues(Value::ofVarchar("ab"), Value::ofVarchar("ab")) == 0);
    // U+00E9 in UTF-8 starts with byte 0xC3: an unsigned byte, so after 'z'.
    CHECK(db::compareValues(Value::ofVarchar("\xC3\xA9"), Value::ofVarchar("z")) > 0);
    CHECK(db::compareValues(Value::ofVarchar("\x80"), Value::ofVarchar("\x7F")) > 0);
}

TEST_CASE("compareValues of INT and VARCHAR is a type error")
{
    const std::optional<db::Error> error =
        errorFrom([] { db::compareValues(Value::ofInt(1), Value::ofVarchar("1")); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Type);
    CHECK(error->message() == "Cannot compare INT with VARCHAR.");
    CHECK(error->line() == 0);
}

TEST_CASE("Value equality needs the same type and the same content")
{
    CHECK(Value::ofInt(7) == Value::ofInt(7));
    CHECK(Value::ofVarchar("7") == Value::ofVarchar("7"));
    CHECK_FALSE(Value::ofInt(7) == Value::ofVarchar("7"));
    CHECK_FALSE(Value::ofInt(7) == Value::ofInt(8));
    CHECK(Value::ofInt(7) != Value::ofVarchar("7"));
}

TEST_CASE("Value operator< is a strict total order usable in std::set")
{
    CHECK(Value::ofInt(-1) < Value::ofInt(0));
    CHECK(Value::ofInt(100) < Value::ofVarchar("1"));
    CHECK_FALSE(Value::ofVarchar("1") < Value::ofInt(100));
    CHECK_FALSE(Value::ofInt(3) < Value::ofInt(3));
    CHECK(Value::ofVarchar("B") < Value::ofVarchar("a"));

    std::set<Value> values;
    values.insert(Value::ofInt(1));
    values.insert(Value::ofVarchar("1"));
    values.insert(Value::ofInt(*db::parseInt32("+01")));
    values.insert(Value::ofVarchar("1"));
    CHECK(values.size() == 2);

    std::set<db::Tuple> tuples;
    tuples.insert({Value::ofInt(1), Value::ofVarchar("x")});
    tuples.insert({Value::ofInt(1), Value::ofVarchar("x")});
    tuples.insert({Value::ofInt(1), Value::ofVarchar("y")});
    CHECK(tuples.size() == 2);
}

// --- parseValue -------------------------------------------------------------

TEST_CASE("parseValue reads INT through parseInt32")
{
    const Column a = intColumn("R", "a");
    CHECK(db::parseValue("42", a) == Value::ofInt(42));
    CHECK(db::parseValue("+007", a) == Value::ofInt(7));
    CHECK(db::parseValue("-2147483648", a) == Value::ofInt(-2147483647 - 1));
}

TEST_CASE("parseValue rejects text that is not an int32 with a type error")
{
    const Column a = intColumn("R", "a");

    const std::optional<db::Error> notNumber = errorFrom([&] { db::parseValue("abc", a); });
    REQUIRE(notNumber.has_value());
    CHECK(notNumber->category() == ErrorCategory::Type);
    CHECK(notNumber->message() == "'abc' is not a valid INT for column R.a.");

    const std::optional<db::Error> tooBig = errorFrom([&] { db::parseValue("2147483648", a); });
    REQUIRE(tooBig.has_value());
    CHECK(tooBig->category() == ErrorCategory::Type);
}

TEST_CASE("parseValue keeps VARCHAR bytes up to the column length")
{
    const Column name = varcharColumn("Emp", "Name", 5);
    CHECK(db::parseValue("Alice", name) == Value::ofVarchar("Alice"));
    CHECK(db::parseValue("", name) == Value::ofVarchar(""));
    CHECK(db::parseValue("007", name) == Value::ofVarchar("007"));

    const std::optional<db::Error> tooLong = errorFrom([&] { db::parseValue("Alicia", name); });
    REQUIRE(tooLong.has_value());
    CHECK(tooLong->category() == ErrorCategory::Schema);
}

// --- Schema::resolve --------------------------------------------------------

TEST_CASE("Schema resolve finds a qualified column by exact qualifier and name")
{
    const Schema emp = empSchema();
    CHECK(emp.size() == 3);
    CHECK(emp.column(1).qualifiedName() == "Emp.Name");
    CHECK(emp.resolve(qualified("Emp", "EID")) == 0);
    CHECK(emp.resolve(qualified("Emp", "DID")) == 2);

    const std::optional<db::Error> wrongQualifier = errorFrom([&] { emp.resolve(qualified("Dept", "DID")); });
    REQUIRE(wrongQualifier.has_value());
    CHECK(wrongQualifier->category() == ErrorCategory::Name);
}

TEST_CASE("Schema resolve finds a bare name only when exactly one column has it")
{
    const Schema joined = db::concatSchemas(empSchema(), deptSchema());
    CHECK(joined.resolve(bare("Name")) == 1);
    CHECK(joined.resolve(bare("DName")) == 4);

    const std::optional<db::Error> ambiguous = errorFrom([&] { joined.resolve(bare("DID")); });
    REQUIRE(ambiguous.has_value());
    CHECK(ambiguous->category() == ErrorCategory::Ambiguity);
    CHECK(ambiguous->message().find("Emp.DID") != std::string::npos);
    CHECK(ambiguous->message().find("Dept.DID") != std::string::npos);

    const std::optional<db::Error> unknown = errorFrom([&] { joined.resolve(bare("Salary")); });
    REQUIRE(unknown.has_value());
    CHECK(unknown->category() == ErrorCategory::Name);
}

TEST_CASE("Schema resolve is case-sensitive")
{
    const Schema emp = empSchema();

    const std::optional<db::Error> lowerQualified = errorFrom([&] { emp.resolve(qualified("emp", "did")); });
    REQUIRE(lowerQualified.has_value());
    CHECK(lowerQualified->category() == ErrorCategory::Name);

    const std::optional<db::Error> lowerBare = errorFrom([&] { emp.resolve(bare("eid")); });
    REQUIRE(lowerBare.has_value());
    CHECK(lowerBare->category() == ErrorCategory::Name);
}

TEST_CASE("ColumnRef toString is qualifier.name or the bare name")
{
    CHECK(qualified("Emp", "DID").toString() == "Emp.DID");
    CHECK(bare("DID").toString() == "DID");
}

TEST_CASE("Schema rejects two columns with the same qualified name")
{
    const std::optional<db::Error> error =
        errorFrom([] { Schema({intColumn("R", "a"), varcharColumn("R", "a", 3)}); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

// --- concatSchemas / requalify ----------------------------------------------

TEST_CASE("concatSchemas keeps the left columns then the right columns (C3)")
{
    const Schema joined = db::concatSchemas(empSchema(), deptSchema());
    CHECK(qualifiedNames(joined) ==
          std::vector<std::string>{"Emp.EID", "Emp.Name", "Emp.DID", "Dept.DID", "Dept.DName"});
    CHECK(joined.column(1).type == Type::Varchar);
    CHECK(joined.column(1).varcharLength == 20);
}

TEST_CASE("concatSchemas of a relation with itself is a schema error")
{
    const std::optional<db::Error> error = errorFrom([] { db::concatSchemas(empSchema(), empSchema()); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

TEST_CASE("requalify replaces every qualifier with the alias (C4)")
{
    const Schema renamed = db::requalify(empSchema(), "e");
    CHECK(qualifiedNames(renamed) == std::vector<std::string>{"e.EID", "e.Name", "e.DID"});
    CHECK(renamed.column(1).type == Type::Varchar);
    CHECK(renamed.column(1).varcharLength == 20);

    // A renamed copy can now be joined with the original.
    CHECK(db::concatSchemas(empSchema(), renamed).size() == 6);
}

TEST_CASE("requalify that makes two columns share a name is a schema error")
{
    const Schema joined = db::concatSchemas(empSchema(), deptSchema());
    const std::optional<db::Error> error = errorFrom([&] { db::requalify(joined, "X"); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

// --- projectSchema ----------------------------------------------------------

TEST_CASE("projectSchema picks columns in the listed order and reports their indices")
{
    std::vector<std::size_t> indices;
    const Schema projected =
        db::projectSchema(empSchema(), {bare("DID"), qualified("Emp", "Name")}, &indices);
    CHECK(qualifiedNames(projected) == std::vector<std::string>{"Emp.DID", "Emp.Name"});
    CHECK(indices == std::vector<std::size_t>{2, 1});

    CHECK(db::projectSchema(empSchema(), {bare("EID")}).size() == 1);
}

TEST_CASE("projectSchema rejects the same column twice (Bonus 1 test 24)")
{
    const std::optional<db::Error> sameText =
        errorFrom([] { db::projectSchema(empSchema(), {bare("Name"), bare("Name")}); });
    REQUIRE(sameText.has_value());
    CHECK(sameText->category() == ErrorCategory::Schema);
    CHECK(sameText->message() == "Duplicate projection attribute 'Emp.Name'.");

    const std::optional<db::Error> differentText =
        errorFrom([] { db::projectSchema(empSchema(), {qualified("Emp", "Name"), bare("Name")}); });
    REQUIRE(differentText.has_value());
    CHECK(differentText->category() == ErrorCategory::Schema);
    CHECK(differentText->message() == "Duplicate projection attribute 'Emp.Name'.");
}

TEST_CASE("projectSchema passes on resolution errors")
{
    const Schema joined = db::concatSchemas(empSchema(), deptSchema());
    const std::optional<db::Error> error = errorFrom([&] { db::projectSchema(joined, {bare("DID")}); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Ambiguity);
}

// --- checkUnionCompatible ---------------------------------------------------

TEST_CASE("checkUnionCompatible needs the same column count")
{
    const Schema r({intColumn("R", "a"), intColumn("R", "b")});
    const Schema s({intColumn("S", "c"), intColumn("S", "d"), intColumn("S", "e")});
    const std::optional<db::Error> error = errorFrom([&] { db::checkUnionCompatible(r, s, "union"); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

TEST_CASE("checkUnionCompatible needs the same type at each position")
{
    const Schema r({intColumn("R", "a"), intColumn("R", "b")});
    const Schema s({intColumn("S", "a"), varcharColumn("S", "b", 10)});
    const std::optional<db::Error> error = errorFrom([&] { db::checkUnionCompatible(r, s, "intersect"); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
}

TEST_CASE("checkUnionCompatible accepts same types whatever the names and lengths")
{
    const Schema r({intColumn("R", "a"), varcharColumn("R", "b", 5)});
    const Schema s({intColumn("S", "x"), varcharColumn("S", "y", 50)});
    CHECK_NOTHROW(db::checkUnionCompatible(r, s, "except"));
}

// --- Error ------------------------------------------------------------------

TEST_CASE("formatError adds the position only when there is one")
{
    const db::Error positioned(ErrorCategory::Type, "Cannot compare INT with VARCHAR.", 3, 7);
    CHECK(db::formatError(positioned) == "error: type: Cannot compare INT with VARCHAR. at line 3, column 7");
    CHECK(std::string(positioned.what()) == db::formatError(positioned));
    CHECK(positioned.message() == "Cannot compare INT with VARCHAR.");
    CHECK(positioned.line() == 3);
    CHECK(positioned.column() == 7);

    const db::Error unpositioned(ErrorCategory::Name, "Unknown attribute 'x'.");
    CHECK(db::formatError(unpositioned) == "error: name: Unknown attribute 'x'.");
    CHECK(std::string(unpositioned.what()) == db::formatError(unpositioned));
    CHECK(unpositioned.line() == 0);
}

TEST_CASE("categoryName gives the lowercase name of every category")
{
    CHECK(db::categoryName(ErrorCategory::Lexical) == "lexical");
    CHECK(db::categoryName(ErrorCategory::Syntax) == "syntax");
    CHECK(db::categoryName(ErrorCategory::Name) == "name");
    CHECK(db::categoryName(ErrorCategory::Ambiguity) == "ambiguity");
    CHECK(db::categoryName(ErrorCategory::Type) == "type");
    CHECK(db::categoryName(ErrorCategory::Aggregate) == "aggregate");
    CHECK(db::categoryName(ErrorCategory::Schema) == "schema");
}

TEST_CASE("Error is caught as std::runtime_error with the formatted text")
{
    try
    {
        throw db::Error(ErrorCategory::Syntax, "Expected ')'.", 1, 12);
    }
    catch (const std::runtime_error& error)
    {
        CHECK(std::string(error.what()) == "error: syntax: Expected ')'. at line 1, column 12");
    }
}

TEST_CASE("kMaxNestingDepth is 1000")
{
    CHECK(db::kMaxNestingDepth == 1000);
}
