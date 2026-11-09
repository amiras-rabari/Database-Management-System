// The `sql` CLI through runSqlCli with string streams: `parse` (chunk 05)
// with and without --explain, `compile` and `check` (chunk 12) against a
// scratch database, the error format, and the exit codes.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"
#include "sql_cli.h"

namespace
{
    namespace fs = std::filesystem;

    // A database in a fresh temp folder whose catalog holds Emp and Dept
    // with the columns of the spec's examples (the sql tests' fixture). No
    // rows: compile and check read only the catalog.
    class TestDatabase
    {
    public:
        explicit TestDatabase(const std::string& name)
            : dir_(fs::temp_directory_path() / "comp3005_sql_cli_tests" / name)
        {
            fs::remove_all(dir_);
            auto engine = db::storage::StorageEngine::open(dir_, 8, db::storage::ReplacementPolicy::Lru);
            engine->catalog().createTable("Emp", db::Schema({{"Emp", "eid", db::Type::Int, 0},
                                                             {"Emp", "name", db::Type::Varchar, 20},
                                                             {"Emp", "age", db::Type::Int, 0},
                                                             {"Emp", "did", db::Type::Int, 0},
                                                             {"Emp", "salary", db::Type::Int, 0},
                                                             {"Emp", "mgr", db::Type::Int, 0},
                                                             {"Emp", "city", db::Type::Varchar, 20}}));
            engine->catalog().createTable("Dept", db::Schema({{"Dept", "did", db::Type::Int, 0},
                                                              {"Dept", "title", db::Type::Varchar, 20},
                                                              {"Dept", "budget", db::Type::Int, 0},
                                                              {"Dept", "city", db::Type::Varchar, 20}}));
            engine->close();
        }

        ~TestDatabase()
        {
            std::error_code ignored;
            fs::remove_all(dir_, ignored);
        }

        TestDatabase(const TestDatabase&) = delete;
        TestDatabase& operator=(const TestDatabase&) = delete;

        std::string dir() const { return dir_.string(); }

        /// Writes `sql` to a file in the folder; returns its path.
        std::string file(const std::string& sql) const
        {
            const fs::path path = dir_ / "q.sql";
            std::ofstream(path, std::ios::binary) << sql;
            return path.string();
        }

    private:
        fs::path dir_;
    };

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

TEST_CASE("sql compile prints example 5.1's logical plan JSON")
{
    const TestDatabase database("compile_5_1");
    const Run result = run({"compile", "--db", database.dir(), "--file",
                            database.file("SELECT name, age FROM Emp WHERE age > 30;"), "--json"});
    CHECK(result.code == 0);
    CHECK(result.err.empty());
    // Compared as JSON values, so only the whitespace may differ from §M3.5.
    CHECK(nlohmann::ordered_json::parse(result.out) ==
          nlohmann::ordered_json::parse(R"({ "op":"project", "columns":["Emp.name","Emp.age"],
              "child": { "op":"select", "condition":"Emp.age > 30",
                         "child": { "op":"scan", "table":"Emp" } } })"));
}

TEST_CASE("sql compile: example 5.7's two unions differ only in the root op")
{
    const TestDatabase database("compile_5_7");
    const Run all = run({"compile", "--db", database.dir(), "--file",
                         database.file("SELECT city FROM Emp UNION ALL SELECT city FROM Dept;")});
    const Run set = run({"compile", "--db", database.dir(), "--file",
                         database.file("SELECT city FROM Emp UNION SELECT city FROM Dept;")});
    REQUIRE(all.code == 0);
    REQUIRE(set.code == 0);
    nlohmann::ordered_json allJson = nlohmann::ordered_json::parse(all.out);
    nlohmann::ordered_json setJson = nlohmann::ordered_json::parse(set.out);
    CHECK(allJson["op"] == "union_all");
    CHECK(setJson["op"] == "union");
    allJson["op"] = "union";
    CHECK(allJson == setJson);
}

TEST_CASE("sql compile: several queries give a JSON array; CREATE and INSERT print nothing")
{
    const TestDatabase database("compile_script");
    const Run result = run({"compile", "--db", database.dir(), "--file",
                            database.file("INSERT INTO Dept VALUES (1, 'x', 500, 'y');\n"
                                          "SELECT name FROM Emp;\nSELECT title FROM Dept;")});
    CHECK(result.code == 0);
    const nlohmann::ordered_json plans = nlohmann::ordered_json::parse(result.out);
    REQUIRE(plans.is_array());
    REQUIRE(plans.size() == 2);
    CHECK(plans[0]["columns"][0] == "Emp.name");
    CHECK(plans[1]["columns"][0] == "Dept.title");

    const Run onlyCreate = run({"compile", "--db", database.dir(), "--file",
                                database.file("CREATE TABLE T (a INT)")});
    CHECK(onlyCreate.code == 0);
    CHECK(onlyCreate.out.empty());
}

TEST_CASE("sql compile: an SQL error goes to stderr, exit 1")
{
    const TestDatabase database("compile_error");
    const Run result = run({"compile", "--db", database.dir(), "--file", database.file("SELECT a FROM Nope")});
    CHECK(result.code == 1);
    CHECK(result.out.empty());
    CHECK(result.err == "error: name: Unknown table 'Nope'. at line 1, column 15\n");
}

TEST_CASE("sql check: exit 0 and no output for a correct query (example 5.4)")
{
    const TestDatabase database("check_ok");
    const Run result = run({"check", "--db", database.dir(), "--file",
                            database.file("SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal\n"
                                          "  FROM Emp\n WHERE age > 25\n GROUP BY did\n"
                                          "HAVING COUNT(*) > 5\n ORDER BY n DESC\n LIMIT 10;")});
    CHECK(result.code == 0);
    CHECK(result.out.empty());
    CHECK(result.err.empty());
}

TEST_CASE("sql check: an SQL error is its output, on stdout, exit 1")
{
    const TestDatabase database("check_errors");
    // Required case 10: the comparison starts at `age`, column 28.
    const Run type = run({"check", "--db", database.dir(), "--file",
                          database.file("SELECT name FROM Emp WHERE age > '30';")});
    CHECK(type.code == 1);
    CHECK(type.err.empty());
    CHECK(type.out == "error: type: Incompatible comparison operand types: INT and VARCHAR. at line 1, column 28\n");

    const Run lexical = run({"check", "--db", database.dir(), "--file",
                             database.file("SELECT name FROM Emp WHERE age # 3")});
    CHECK(lexical.code == 1);
    CHECK(lexical.out.rfind("error: lexical: ", 0) == 0);

    const Run syntax = run({"check", "--db", database.dir(), "--file", database.file("SELECT FROM Emp")});
    CHECK(syntax.code == 1);
    CHECK(syntax.out.rfind("error: syntax: ", 0) == 0);

    // A file that cannot be read is still exit 1 (DECISIONS 2026-11-08).
    const Run missing = run({"check", "--db", database.dir(), "--file", database.dir() + "/missing.sql"});
    CHECK(missing.code == 1);
    CHECK(missing.err.rfind("error: ", 0) == 0);
}

TEST_CASE("sql compile/check: --catalog is --db; --frames is accepted; bad lines exit 2")
{
    const TestDatabase database("options");
    const std::string query = database.file("SELECT name FROM Emp");
    const Run withDb = run({"compile", "--db", database.dir(), "--file", query});
    const Run withCatalog = run({"compile", "--catalog", database.dir(), "--file", query, "--frames", "4"});
    CHECK(withDb.code == 0);
    CHECK(withCatalog.code == 0);
    CHECK(withCatalog.out == withDb.out);

    const std::vector<std::vector<std::string>> badLines = {
        {"check", "--db", database.dir()},                      // missing --file
        {"compile", "--file", query},                           // missing --db
        {"check", "--db", database.dir(), "--file"},            // --file without its path
        {"check", "--db", database.dir(), "--file", query, "--frames", "0"},
        {"compile", "--db", database.dir(), "--file", query, "--explain"}, // unknown option
    };
    for (const std::vector<std::string>& args : badLines)
    {
        const Run result = run(args);
        CHECK(result.code == 2);
        CHECK(result.out.empty());
        CHECK(result.err.rfind("usage error: ", 0) == 0);
    }
}
