// The CLI's Bonus 1 relation-text loader: column typing from the data, its
// errors and their categories, duplicate rows kept (bags) and removed only
// by distinct, and the program reader that hands the query to the library
// parser with real line numbers.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/operators.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/tuple_source.h"
#include "db/common/value.h"
#include "program.h"
#include "relation_loader.h"
#include "vector_source.h"

namespace
{

    using db::ErrorCategory;

    // Loads `relations` followed by the query `R`.
    std::vector<ra::Relation> load(const std::string& relations)
    {
        return ra::parseProgram(relations + "\nR", false).relations;
    }

    // "R.a INT, R.b VARCHAR(3)" in schema order.
    std::string describe(const db::Schema& schema)
    {
        std::string out;
        for (const db::Column& column : schema.columns())
        {
            out += (out.empty() ? "" : ", ") + column.qualifiedName() + " " + std::string(db::typeName(column.type));
            if (column.type == db::Type::Varchar)
            {
                out += "(" + std::to_string(column.varcharLength) + ")";
            }
        }
        return out;
    }

    std::vector<std::string> rowsOf(const ra::Relation& relation)
    {
        std::vector<std::string> rows;
        for (const db::Tuple& tuple : relation.rows)
        {
            std::string row;
            for (const db::Value& value : tuple)
            {
                row += (row.empty() ? "" : " ") + value.toText();
            }
            rows.push_back(row);
        }
        return rows;
    }

    std::optional<db::Error> programError(const std::string& text, bool treeOnly = false)
    {
        try
        {
            ra::parseProgram(text, treeOnly);
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    std::optional<db::Error> loadError(const std::string& relations)
    {
        return programError(relations + "\nR");
    }

} // namespace

TEST_CASE("columns are qualified by the relation name and typed from the first row")
{
    const auto relations = load("R (a, b, c) = { 1, x, 'yy' \n -2, 'z', abc }\nS (d) = { 7 }");
    REQUIRE(relations.size() == 2);
    CHECK(relations[0].name == "R");
    CHECK(describe(relations[0].schema) == "R.a INT, R.b VARCHAR(1), R.c VARCHAR(3)");
    CHECK(rowsOf(relations[0]) == std::vector<std::string>{"1 x yy", "-2 z abc"});
    CHECK(relations[0].rows[1][0] == db::Value::ofInt(-2));
    CHECK(describe(relations[1].schema) == "S.d INT");
}

TEST_CASE("a quoted integer is a VARCHAR")
{
    CHECK(describe(load("R (a) = { '1' }")[0].schema) == "R.a VARCHAR(1)");
}

TEST_CASE("VARCHAR length is the longest value in bytes, at least 1")
{
    CHECK(describe(load("R (a) = { 'ab' \n abcd \n x }")[0].schema) == "R.a VARCHAR(4)");
    CHECK(describe(load("R (a) = { '' }")[0].schema) == "R.a VARCHAR(1)");
    CHECK(describe(load("R (a) = { '\xC3\xA9' }")[0].schema) == "R.a VARCHAR(2)");
    CHECK(describe(load("R (a) = { '" + std::string(255, 'v') + "' }")[0].schema) == "R.a VARCHAR(255)");
}

TEST_CASE("a value longer than 255 bytes is a schema error")
{
    const auto error = loadError("R (a) = { x \n '" + std::string(256, 'v') + "' }");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
    CHECK(error->line() == 2);
}

TEST_CASE("an empty relation has every column VARCHAR(255)")
{
    const auto relations = load("R (a, b) = { }");
    CHECK(describe(relations[0].schema) == "R.a VARCHAR(255), R.b VARCHAR(255)");
    CHECK(relations[0].rows.empty());
}

TEST_CASE("a later row with the other kind of value is a type error")
{
    const auto stringThenInt = loadError("R (a, b) = { 1, x \n 2, 3 }");
    REQUIRE(stringThenInt.has_value());
    CHECK(stringThenInt->category() == ErrorCategory::Type);
    CHECK(stringThenInt->message().rfind("Type mismatch in relation 'R', row 2, attribute 'b'.", 0) == 0);
    CHECK(stringThenInt->line() == 2);
    CHECK(stringThenInt->column() == 5);

    const auto intThenString = loadError("R (a) = { 1 \n '2' }");
    REQUIRE(intThenString.has_value());
    CHECK(intThenString->category() == ErrorCategory::Type);
}

TEST_CASE("an unquoted integer outside the INT range is a lexical error")
{
    const auto relations = load("R (a) = { 2147483647 \n -2147483648 \n +007 }");
    CHECK(describe(relations[0].schema) == "R.a INT");
    CHECK(rowsOf(relations[0]) == std::vector<std::string>{"2147483647", "-2147483648", "7"});

    const auto tooBig = loadError("R (a) = { 2147483648 }");
    REQUIRE(tooBig.has_value());
    CHECK(tooBig->category() == ErrorCategory::Lexical);
    const auto tooSmall = loadError("R (a) = { x \n -2147483649 }");
    REQUIRE(tooSmall.has_value());
    CHECK(tooSmall->category() == ErrorCategory::Lexical);
}

TEST_CASE("an attribute named twice is a schema error")
{
    const auto error = loadError("R (a, b, a) = { 1, 2, 3 }");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
    CHECK(error->message() == "Duplicate attribute 'a' in relation 'R'.");
}

TEST_CASE("a row with the wrong number of values is a schema error")
{
    const auto error = loadError("R (a, b) = { 1, 2 \n 3 }");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
    CHECK(error->line() == 2);
}

TEST_CASE("a relation defined twice is a name error")
{
    const auto error = loadError("R (a) = { 1 }\nR (b) = { 2 }");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
}

TEST_CASE("malformed relation text is a syntax or lexical error with its position")
{
    const auto syntax = loadError("R (a) = { 1, }");
    REQUIRE(syntax.has_value());
    CHECK(syntax->category() == ErrorCategory::Syntax);
    CHECK(syntax->line() == 1);

    const auto lexical = loadError("R (a) = {\n 'open\n}");
    REQUIRE(lexical.has_value());
    CHECK(lexical->category() == ErrorCategory::Lexical);
    CHECK(lexical->line() == 2);
}

TEST_CASE("duplicate rows are kept at load (bags)")
{
    const auto relations = load("R (a, b) = { 7, x \n 007, 'x' \n +7, x \n 8, x }");
    CHECK(rowsOf(relations[0]) == std::vector<std::string>{"7 x", "7 x", "7 x", "8 x"});
}

TEST_CASE("distinct over loaded rows treats 7 / 007 / +7 and bare / quoted x as the same values")
{
    const ra::Program program =
        ra::parseProgram("R (a, b) = { 7, x \n 007, 'x' \n +7, x \n 8, x }\ndistinct(R)", false);
    const ra::Relation& relation = program.relations.at(0);
    const auto openTable = [&relation](const std::string&) -> db::TupleSourcePtr
    {
        return std::make_unique<ra::VectorSource>(relation.schema, relation.rows);
    };
    db::OperatorCounters counters;
    const db::TupleSourcePtr source = db::buildOperators(program.query, openTable, counters);
    std::vector<std::string> rows;
    db::Tuple tuple;
    while (source->next(tuple))
    {
        rows.push_back(tuple[0].toText() + " " + tuple[1].toText());
    }
    CHECK(rows == std::vector<std::string>{"7 x", "8 x"});
}

TEST_CASE("the query follows the relations, also on the same line")
{
    CHECK(ra::parseProgram("R (a) = { 1 } select[a = 1](R)", false).query ==
          db::PlanNode::select(db::parseCondition("a = 1"), db::PlanNode::scan("R")));
    CHECK(ra::parseProgram("R (a) = { 1 }\n(R union R)", false).query ==
          db::PlanNode::unionOf(db::PlanNode::scan("R"), db::PlanNode::scan("R")));
}

TEST_CASE("a query starting with distinct( is the query, not another relation")
{
    const ra::Program program = ra::parseProgram("R (a) = { 1 }\ndistinct(R)", false);
    CHECK(program.relations.size() == 1);
    CHECK(program.query == db::PlanNode::distinct(db::PlanNode::scan("R")));
    // Like the other RA keywords, distinct may still name an attribute.
    CHECK(describe(load("R (distinct) = { 1 }")[0].schema) == "R.distinct INT");
}

TEST_CASE("a query error keeps its real line and column after the relations")
{
    const std::string relations = "R (a, b) = {\n  1, x\n  2, y\n}\n\n";
    const auto syntax = programError(relations + "select[a = 1](R");
    REQUIRE(syntax.has_value());
    CHECK(syntax->category() == ErrorCategory::Syntax);
    CHECK(syntax->line() == 6);
    CHECK(syntax->column() == 16);

    const auto lexical = programError(relations + "  select[b = 'x](R)");
    REQUIRE(lexical.has_value());
    CHECK(lexical->category() == ErrorCategory::Lexical);
    CHECK(lexical->line() == 6);
    CHECK(lexical->column() == 14);
}

TEST_CASE("tree-only mode reads the whole text as a query")
{
    const ra::Program program = ra::parseProgram("R minus S", true);
    CHECK(program.relations.empty());
    CHECK(program.query == db::PlanNode::except(db::PlanNode::scan("R"), db::PlanNode::scan("S")));
    // `{` is not part of the query language.
    const auto error = programError("R (a) = { 1 }", true);
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Lexical);
}
