// The shell through runShell with string streams: chunk 01 (the input split
// into statements, CREATE / INSERT / LOAD / SET / ANALYZE), chunk 02
// (SELECT, EXPLAIN, EXPLAIN ANALYZE and the numbers of each query) and
// chunk 03 (the prompt and --json).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "db/shell/shell.h"
#include "db/storage/catalog.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::ordered_json;

    // What one runShell call returned and printed.
    struct Result
    {
        int code = 0;
        std::string out;
        std::string err;
    };

    // A fresh database folder per test, deleted at the end.
    class TestFolder
    {
    public:
        explicit TestFolder(const std::string& name)
            : dir_(fs::temp_directory_path() / "comp3005_shell_tests" / name)
        {
            fs::remove_all(dir_);
            fs::create_directories(dir_);
        }

        ~TestFolder()
        {
            std::error_code ignored;
            fs::remove_all(dir_, ignored);
        }

        TestFolder(const TestFolder&) = delete;
        TestFolder& operator=(const TestFolder&) = delete;

        std::string dir() const { return dir_.string(); }

        /// Writes `text` to the file `name` in the folder; returns its path.
        std::string file(const std::string& name, const std::string& text) const
        {
            const fs::path path = dir_ / name;
            std::ofstream(path, std::ios::binary) << text;
            return path.string();
        }

        /// Runs the shell on the database folder `db` inside this folder,
        /// with `input` as the typed input and the extra arguments `args`.
        Result run(const std::string& input, std::vector<std::string> args = {}) const
        {
            args.insert(args.begin(), {"--db", (dir_ / "db").string()});
            std::istringstream in(input);
            std::ostringstream out;
            std::ostringstream err;
            Result result;
            result.code = db::shell::runShell(args, in, out, err);
            result.out = out.str();
            result.err = err.str();
            return result;
        }

        /// Runs `script` from a file with --file (no prompt on err).
        Result script(const std::string& script, std::vector<std::string> args = {}) const
        {
            args.insert(args.end(), {"--file", file("script.sql", script)});
            return run("", args);
        }

        /// The catalog entry of `table` (nullopt = no such table), read
        /// with a fresh engine after the shell has closed its own.
        std::optional<db::storage::TableInfo> table(const std::string& table) const
        {
            auto engine = db::storage::StorageEngine::open(dir_ / "db", 4, db::storage::ReplacementPolicy::Lru);
            std::optional<db::storage::TableInfo> info = engine->catalog().getTable(table);
            engine->close();
            return info;
        }

    private:
        fs::path dir_;
    };

    // The lines of `text` (each ends with "\n").
    std::vector<std::string> linesOf(const std::string& text)
    {
        std::vector<std::string> lines;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        return lines;
    }

    bool contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }

    // Exec's sort-merge example (exec SPEC §M2.3): R.b = 1, 2, 2, 3 and
    // S.b = 2, 2, 4, so R.b = S.b has 2 x 2 = 4 matches. Plus an empty E.
    const char* const kJoinTables = "CREATE TABLE R (a INT, b INT);\n"
                                    "INSERT INTO R VALUES (1, 1);\n"
                                    "INSERT INTO R VALUES (2, 2);\n"
                                    "INSERT INTO R VALUES (3, 2);\n"
                                    "INSERT INTO R VALUES (4, 3);\n"
                                    "CREATE TABLE S (a INT, b INT);\n"
                                    "INSERT INTO S VALUES (10, 2);\n"
                                    "INSERT INTO S VALUES (20, 2);\n"
                                    "INSERT INTO S VALUES (30, 4);\n"
                                    "CREATE TABLE E (a INT);\n";

    // A folder holding the join tables, ready for queries at B = 4.
    void makeJoinTables(const TestFolder& folder)
    {
        const Result setup = folder.script(kJoinTables, {"--frames", "4"});
        REQUIRE(setup.code == 0);
        REQUIRE(setup.err.empty());
    }

    // The one JSON line of a --json run with one statement.
    Json onlyJsonLine(const Result& result)
    {
        const std::vector<std::string> lines = linesOf(result.out);
        REQUIRE(lines.size() == 1);
        return Json::parse(lines[0]);
    }
} // namespace

// ---------------------------------------------------------------------------
// Chunk 01: the entry point, statement splitting, commands that change data
// ---------------------------------------------------------------------------

TEST_CASE("CREATE then INSERT in one script: compiled one statement at a time")
{
    const TestFolder folder("create_insert");
    const Result result = folder.script("CREATE TABLE R (a INT, b INT); INSERT INTO R VALUES (1, 1);");
    CHECK(result.code == 0);
    CHECK(result.err.empty());
    CHECK(result.out == "created table R\ninserted 1 row into R\n");
    CHECK(folder.table("R")->numTuples == 1);
}

TEST_CASE("LOAD reads a CSV file through storage's loader")
{
    const TestFolder folder("load");
    const std::string csv = folder.file("r.csv", "1,1\n2,2\n3,2\n4,3\n");
    const Result result = folder.script("CREATE TABLE R (a INT, b INT);\nLOAD R FROM '" + csv + "';");
    CHECK(result.code == 0);
    CHECK(result.out == "created table R\nloaded 4 rows into R\n");
    CHECK(folder.table("R")->numTuples == 4);

    // The path must be a quoted string.
    const Result unquoted = folder.script("LOAD R FROM r.csv;");
    CHECK(unquoted.code == 1);
    CHECK(contains(unquoted.err, "error: syntax:"));
}

TEST_CASE("SET frames = n needs n of at least 3, like --frames")
{
    const TestFolder folder("set_frames");
    const Result ok = folder.script("SET frames = 4;");
    CHECK(ok.code == 0);
    CHECK(ok.out == "frames = 4\n");

    const Result tooSmall = folder.script("SET frames = 2;");
    CHECK(tooSmall.code == 1);
    CHECK(tooSmall.out.empty());
    CHECK(contains(tooSmall.err, "error: syntax:"));

    for (const char* wrong : {"SET frames 4;", "SET frames = x;", "SET pages = 4;"})
        CHECK(contains(folder.script(wrong).err, "error: syntax:"));

    // On the command line it is a usage error: exit 2, nothing opened.
    const Result usage = folder.script("SET frames = 4;", {"--frames", "2"});
    CHECK(usage.code == 2);
    CHECK(contains(usage.err, "usage error:"));
}

TEST_CASE("usage errors: missing --db, unknown option, missing value")
{
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    CHECK(db::shell::runShell({}, in, out, err) == 2);
    CHECK(db::shell::runShell({"--db", "x", "--bogus"}, in, out, err) == 2);
    CHECK(db::shell::runShell({"--db"}, in, out, err) == 2);
    CHECK(out.str().empty());
}

TEST_CASE("ANALYZE twice gives the same statistics")
{
    const TestFolder folder("analyze");
    makeJoinTables(folder);
    const Result result = folder.script("ANALYZE R;\nANALYZE R;");
    CHECK(result.code == 0);
    const std::vector<std::string> lines = linesOf(result.out);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == lines[1]);
    CHECK(lines[0].rfind("analyzed R: 4 rows, 1 pages, width ", 0) == 0);
    CHECK(folder.table("R")->stats.has_value());

    CHECK(contains(folder.script("ANALYZE;").err, "error: syntax:"));
    CHECK(contains(folder.script("ANALYZE Nope;").err, "error: name: Unknown table 'Nope'."));
}

TEST_CASE("splitting: semicolons in strings and comments, a missing last semicolon")
{
    const TestFolder folder("splitting");
    REQUIRE(folder.script("CREATE TABLE T (s VARCHAR(5));").code == 0);

    // The ';' inside the string does not end the statement.
    CHECK(folder.script("INSERT INTO T VALUES ('a;b');").code == 0);
    CHECK(folder.table("T")->numTuples == 1);

    // The comment hides its ';': one INSERT, not two statements.
    CHECK(folder.script("INSERT INTO T VALUES ('x') -- ;\n;").code == 0);
    CHECK(folder.table("T")->numTuples == 2);

    // The last statement may have no ';'.
    CHECK(folder.script("INSERT INTO T VALUES ('y')").code == 0);
    CHECK(folder.table("T")->numTuples == 3);

    // An unterminated string fails only its own piece (line 1); line 2 runs.
    const Result lexical = folder.script("INSERT INTO T VALUES ('abc;\nINSERT INTO T VALUES ('ok');");
    CHECK(lexical.code == 1);
    CHECK(lexical.err == "error: lexical: unterminated string at line 1, column 23\n");
    CHECK(folder.table("T")->numTuples == 4);
}

TEST_CASE("a failing statement: position in the whole script, the rest still runs")
{
    const TestFolder folder("error_position");
    const Result result = folder.script("CREATE TABLE R (a INT);\n"
                                        "INSERT INTO R VALUES (1);\n"
                                        "\n"
                                        "INSERT INTO R VALUES (1); INSERT INTO R VALUES ('x');\n"
                                        "INSERT INTO R VALUES (2);");
    CHECK(result.code == 1);
    // The bad literal 'x' is at line 4, column 49 of the script.
    CHECK(result.err == "error: type: Value 'x' is VARCHAR, but column 'R.a' is INT. at line 4, column 49\n");
    CHECK(folder.table("R")->numTuples == 3);

    // An error on a later line of a statement keeps its own column.
    const Result multiLine = folder.script("SELECT *\nFROM Nope;");
    CHECK(multiLine.err == "error: name: Unknown table 'Nope'. at line 2, column 6\n");
}

TEST_CASE("the database stays on disk: a second run sees the table and its rows")
{
    const TestFolder folder("reopen");
    const std::string csv = folder.file("r.csv", "1,1\n2,2\n3,2\n4,3\n");
    REQUIRE(folder.script("CREATE TABLE R (a INT, b INT); LOAD R FROM '" + csv + "';").code == 0);
    const Result again = folder.script("SELECT * FROM R;");
    CHECK(again.code == 0);
    CHECK(contains(again.out, "4 rows\n"));
}

// ---------------------------------------------------------------------------
// Chunk 02: SELECT, EXPLAIN, EXPLAIN ANALYZE
// ---------------------------------------------------------------------------

TEST_CASE("exec's sort-merge example: 4 rows, comma join = JOIN ON")
{
    const TestFolder folder("join");
    makeJoinTables(folder);
    const Result joinOn = folder.script("SELECT * FROM R JOIN S ON R.b = S.b;", {"--frames", "4"});
    const Result comma = folder.script("SELECT * FROM R, S WHERE R.b = S.b;", {"--frames", "4"});
    CHECK(joinOn.code == 0);
    CHECK(joinOn.err.empty());
    CHECK(comma.out == joinOn.out);

    // Output format: the header, one line per row, the row count, nothing else.
    const std::vector<std::string> lines = linesOf(joinOn.out);
    REQUIRE(lines.size() == 6);
    CHECK(lines[0] == "R.a | R.b | S.a | S.b");
    CHECK(lines[5] == "4 rows");
    for (std::size_t i = 1; i <= 4; ++i)
        CHECK(lines[i].find(" | 2 | ") != std::string::npos);
}

TEST_CASE("bags: UNION ALL keeps duplicates; UNION, INTERSECT, EXCEPT remove them")
{
    const TestFolder folder("set_ops");
    makeJoinTables(folder);
    const auto rowsOf = [&folder](const std::string& query) {
        return linesOf(folder.script(query, {"--frames", "4"}).out);
    };
    CHECK(rowsOf("SELECT R.b FROM R UNION ALL SELECT S.b FROM S;").back() == "7 rows");
    CHECK(rowsOf("SELECT R.b FROM R UNION SELECT S.b FROM S;").back() == "4 rows");
    CHECK(rowsOf("SELECT R.b FROM R INTERSECT SELECT S.b FROM S;") ==
          std::vector<std::string>{"R.b", "2", "1 row"});
    CHECK(rowsOf("SELECT R.b FROM R EXCEPT SELECT S.b FROM S;") ==
          std::vector<std::string>{"R.b", "1", "3", "2 rows"});
}

TEST_CASE("aggregates: COUNT(*) of an empty table is one row 0; AVG prints 4 decimals")
{
    const TestFolder folder("aggregates");
    makeJoinTables(folder);
    CHECK(folder.script("SELECT COUNT(*) FROM E;").out == "count_star\n0\n1 row\n");
    CHECK(folder.script("SELECT AVG(R.b) FROM R;").out == "avg_R_b\n2.0000\n1 row\n");
}

TEST_CASE("a cross product larger than both inputs completes at B = 4")
{
    const TestFolder folder("cross");
    makeJoinTables(folder);
    const Result result = folder.script("SELECT * FROM R, S;", {"--frames", "4"});
    CHECK(result.code == 0);
    CHECK(linesOf(result.out).back() == "12 rows");
}

TEST_CASE("query errors: name and type, nothing on out, the next statement runs")
{
    const TestFolder folder("query_errors");
    makeJoinTables(folder);
    const Result result = folder.script("SELECT * FROM Nope;\nSELECT * FROM R WHERE R.a = 'x';\nSELECT * FROM E;");
    CHECK(result.code == 1);
    const std::vector<std::string> errors = linesOf(result.err);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].rfind("error: name:", 0) == 0);
    CHECK(errors[1].rfind("error: type:", 0) == 0);
    CHECK(result.out == "E.a\n0 rows\n");
}

TEST_CASE("EXPLAIN prints opt's plan with estimates and runs nothing")
{
    const TestFolder folder("explain");
    makeJoinTables(folder);
    const Result joinOn = folder.script("EXPLAIN SELECT * FROM R JOIN S ON R.b = S.b;", {"--frames", "4"});
    const Result comma = folder.script("explain SELECT * FROM R, S WHERE R.b = S.b;", {"--frames", "4"});
    CHECK(joinOn.code == 0);
    CHECK(comma.out == joinOn.out);
    const std::vector<std::string> lines = linesOf(joinOn.out);
    // opt's plan: project (restores the written column order), the join,
    // and its two scans (the DP puts S outer).
    REQUIRE(lines.size() == 4);
    for (const std::string& line : lines)
    {
        CHECK(contains(line, "est_rows="));
        CHECK(contains(line, "est_cost="));
    }
    CHECK(contains(lines[2], "seqscan S"));
    CHECK(contains(lines[3], "seqscan R"));

    CHECK(contains(folder.script("EXPLAIN CREATE TABLE X (a INT);").err, "error: syntax: EXPLAIN needs a query"));
    CHECK_FALSE(folder.table("X").has_value());
}

TEST_CASE("EXPLAIN ANALYZE: estimates and real counts side by side, no rows")
{
    const TestFolder folder("explain_analyze");
    makeJoinTables(folder);
    const Result result = folder.script("EXPLAIN ANALYZE SELECT * FROM R JOIN S ON R.b = S.b;", {"--frames", "4"});
    CHECK(result.code == 0);
    const std::vector<std::string> lines = linesOf(result.out);
    REQUIRE(lines.size() == 5);
    CHECK(contains(lines[0], "actual_rows=4 "));
    CHECK(contains(lines[2], "seqscan S"));
    CHECK(contains(lines[2], "actual_rows=3 "));
    CHECK(contains(lines[3], "seqscan R"));
    CHECK(contains(lines[3], "actual_rows=4 "));
    CHECK(lines[4].rfind("rows=4 page_io=", 0) == 0);
    CHECK(contains(lines[4], " actual_root_rows=4"));
}

// ---------------------------------------------------------------------------
// Chunk 03: the prompt and --json
// ---------------------------------------------------------------------------

TEST_CASE("typed input: results on out, the prompt on err")
{
    const TestFolder folder("prompt");
    const Result oneLine = folder.run("CREATE TABLE T (a INT);\nINSERT INTO T VALUES (7);\nSELECT * FROM T;\n");
    CHECK(oneLine.code == 0);
    CHECK(oneLine.out == "created table T\ninserted 1 row into T\nT.a\n7\n1 row\n");
    CHECK(contains(oneLine.err, "db> "));
    CHECK_FALSE(contains(oneLine.out, "db> "));

    // A statement over two lines gives the same result, with the
    // continuation prompt while it is still open.
    const Result twoLines = folder.run("SELECT *\nFROM T;\n");
    CHECK(twoLines.out == "T.a\n7\n1 row\n");
    CHECK(contains(twoLines.err, "...> "));
    CHECK_FALSE(contains(twoLines.out, "...> "));
}

TEST_CASE("--json: one line of numbers per statement, no rows")
{
    const TestFolder folder("json");
    const std::string script = "CREATE TABLE T (a INT);\nINSERT INTO T VALUES (7);\nSELECT * FROM T;\n"
                               "SELECT * FROM Nope;\n";
    const Result result = folder.script(script, {"--json", "--frames", "4"});
    CHECK(result.code == 1);
    const std::vector<std::string> lines = linesOf(result.out);
    REQUIRE(lines.size() == 4);
    CHECK(Json::parse(lines[0]) == Json{{"statement", 1}, {"ok", true}});
    CHECK(Json::parse(lines[1]) == Json{{"statement", 2}, {"ok", true}});

    const Json select = Json::parse(lines[2]);
    CHECK(select["statement"] == 3);
    CHECK(select["ok"] == true);
    CHECK(select["rows"] == 1);
    CHECK(select["page_reads"] == 2); // 1 data page + 1 directory page, cold pool
    CHECK(select["page_writes"] == 0);
    CHECK(select["wall_ms"].get<double>() >= 0.0);
    CHECK(select["est_root_rows"].get<std::int64_t>() >= 1);
    CHECK(select["actual_root_rows"] == 1);
    CHECK(select["operators"] == Json::parse(R"([{"label":"seqscan T","est_rows":1,"tuples_out":1}])"));

    const Json failed = Json::parse(lines[3]);
    CHECK(failed["ok"] == false);
    CHECK(failed["error"] == "error: name: Unknown table 'Nope'. at line 4, column 15");
}

TEST_CASE("--json: the same query twice gives the same counters (cold pool)")
{
    const TestFolder folder("json_determinism");
    makeJoinTables(folder);
    const std::string query = "SELECT * FROM R JOIN S ON R.b = S.b;\n";
    const Result result = folder.script(query + query, {"--json", "--frames", "4"});
    const std::vector<std::string> lines = linesOf(result.out);
    REQUIRE(lines.size() == 2);
    Json first = Json::parse(lines[0]);
    Json second = Json::parse(lines[1]);
    first.erase("wall_ms");
    first.erase("statement");
    second.erase("wall_ms");
    second.erase("statement");
    CHECK(first == second);
    CHECK(first["rows"] == 4);

    // A second run of the whole script gives the same numbers too.
    const Result again = folder.script(query, {"--json", "--frames", "4"});
    Json third = onlyJsonLine(again);
    third.erase("wall_ms");
    third.erase("statement");
    CHECK(third == first);
}

TEST_CASE("--json: estimated against actual rows at the root after ANALYZE")
{
    const TestFolder folder("json_estimates");
    makeJoinTables(folder);
    REQUIRE(folder.script("ANALYZE R;").code == 0);
    const Json select = onlyJsonLine(folder.script("SELECT * FROM R;", {"--json"}));
    CHECK(select["est_root_rows"] == 4);
    CHECK(select["actual_root_rows"] == 4);

    const Json analyze =
        onlyJsonLine(folder.script("EXPLAIN ANALYZE SELECT * FROM R JOIN S ON R.b = S.b;", {"--json", "--frames", "4"}));
    CHECK(analyze["rows"] == 4);
    REQUIRE(analyze["operators"].size() == 4);
    CHECK(analyze["operators"][0]["tuples_out"] == 4);
}
