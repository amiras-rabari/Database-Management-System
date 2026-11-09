// The `sql` CLI (sql chunk 05) through runSqlCli with string streams:
// `parse` with and without --explain, the error format, and the exit codes.

#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "sql_cli.h"

namespace
{
    // The result of one run: exit code and both output streams.
    struct Run
    {
        int code = 0;
        std::string out;
        std::string err;
    };

    Run run(const std::vector<std::string>& args)
    {
        std::ostringstream out;
        std::ostringstream err;
        const int code = db::apps::runSqlCli(args, out, err);
        return Run{code, out.str(), err.str()};
    }
} // namespace

TEST_CASE("sql parse --explain prints the tree of example 5.1")
{
    const Run result = run({"parse", "--query", "SELECT name, age FROM Emp WHERE age > 30;", "--explain"});
    CHECK(result.code == 0);
    CHECK(result.err.empty());
    CHECK(result.out == "select_block\n"
                        "  item: name\n"
                        "  item: age\n"
                        "  from\n"
                        "    table: Emp\n"
                        "  where: age > 30\n");
}

TEST_CASE("sql parse --explain prints one tree per statement")
{
    const Run result = run({"parse", "--explain", "--query", "INSERT INTO R VALUES (1); SELECT a FROM R"});
    CHECK(result.code == 0);
    CHECK(result.out == "insert R\n"
                        "  values: 1\n"
                        "select_block\n"
                        "  item: a\n"
                        "  from\n"
                        "    table: R\n");
}

TEST_CASE("sql parse without --explain prints nothing when the text parses")
{
    const Run result = run({"parse", "--query", "SELECT a FROM R"});
    CHECK(result.code == 0);
    CHECK(result.out.empty());
    CHECK(result.err.empty());
}

TEST_CASE("sql parse: an SQL error exits 1 with the one error format")
{
    // FROM starts at column 8, where the parser wanted a select item.
    const Run syntax = run({"parse", "--query", "SELECT FROM R", "--explain"});
    CHECK(syntax.code == 1);
    CHECK(syntax.out.empty());
    CHECK(syntax.err == "error: syntax: expected a column, an aggregate or '*' but found 'FROM' at line 1, column 8\n");

    // An error in the second statement: nothing of the first is printed.
    const Run lexical = run({"parse", "--query", "SELECT a FROM R;\nSELECT a FROM R WHERE a != 1", "--explain"});
    CHECK(lexical.code == 1);
    CHECK(lexical.out.empty());
    CHECK(lexical.err == "error: lexical: unexpected character '!' at line 2, column 25\n");
}

TEST_CASE("sql: a bad command line is a usage error, exit 2")
{
    const std::vector<std::vector<std::string>> badLines = {
        {},                                   // no command
        {"parse"},                            // missing --query
        {"parse", "--explain"},               // still missing --query
        {"parse", "--query"},                 // --query without its text
        {"parse", "--query", "SELECT a FROM R", "--json"}, // unknown option
        {"run", "--query", "SELECT a FROM R"}, // unknown command
    };
    for (const std::vector<std::string>& args : badLines)
    {
        const Run result = run(args);
        CHECK(result.code == 2);
        CHECK(result.out.empty());
        CHECK(result.err.rfind("usage error: ", 0) == 0);
        CHECK(result.err.find("sql parse --query") != std::string::npos);
    }
    CHECK(run({"parse"}).err.rfind("usage error: parse needs --query \"SQL\"\n", 0) == 0);
}
