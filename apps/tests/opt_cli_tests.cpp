// The `opt` CLI (opt chunk 14) through runOptCli with string streams: the
// four commands of §M4.6, the report switches, the exit codes, and the main
// test of the module: a plan printed by `opt plan` runs in exec unedited.

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

#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/catalog.h"
#include "db/storage/heap_file.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"
#include "exec_cli.h"
#include "opt_cli.h"

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::ordered_json;

    // A database in a fresh temp folder with three small tables:
    // R(a, b): 200 rows, a = i, b = i mod 10;
    // S(b, c): 50 rows, b = j, c = j mod 5;
    // T(c, d): 20 rows, c = k, d = k.
    // So R.b = S.b matches every R row exactly once (200 rows), and then
    // S.c = T.c matches each of those exactly once again (200 rows).
    class TestDatabase
    {
    public:
        explicit TestDatabase(const std::string& name)
            : dir_(fs::temp_directory_path() / "comp3005_opt_cli_tests" / name)
        {
            fs::remove_all(dir_);
            auto engine = db::storage::StorageEngine::open(dir_, 8, db::storage::ReplacementPolicy::Lru);
            addTable(*engine, "R", "a", "b", 200, 1000000, 10);
            addTable(*engine, "S", "b", "c", 50, 1000000, 5);
            addTable(*engine, "T", "c", "d", 20, 1000000, 1000000);
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

        /// Writes `text` to the file `name` in the folder; returns its path.
        std::string file(const std::string& name, const std::string& text) const
        {
            const fs::path path = dir_ / name;
            std::ofstream(path, std::ios::binary) << text;
            return path.string();
        }

    private:
        // Table `name`(first, second) with `rows` rows: row i holds
        // (i mod firstMod, i mod secondMod); a large mod keeps i itself.
        static void addTable(db::storage::StorageEngine& engine, const std::string& name, const std::string& first,
                             const std::string& second, std::int32_t rows, std::int32_t firstMod,
                             std::int32_t secondMod)
        {
            const db::Schema schema({{name, first, db::Type::Int, 0}, {name, second, db::Type::Int, 0}});
            engine.catalog().createTable(name, schema);
            db::storage::HeapFile heap = engine.openHeapFile(name);
            for (std::int32_t i = 0; i < rows; ++i)
                heap.insert({db::Value::ofInt(i % firstMod), db::Value::ofInt(i % secondMod)});
            engine.catalog().updateCounts(name, heap.numDataPages(), static_cast<std::uint64_t>(rows));
        }

        fs::path dir_;
    };

    struct Run
    {
        int code = 0;
        std::string out;
        std::string err;
    };

    Run opt(const std::vector<std::string>& args)
    {
        std::ostringstream out, err;
        Run run;
        run.code = db::apps::runOptCli(args, out, err);
        run.out = out.str();
        run.err = err.str();
        return run;
    }

    Run exec(const std::vector<std::string>& args)
    {
        std::ostringstream out, err;
        Run run;
        run.code = db::apps::runExecCli(args, out, err);
        run.out = out.str();
        run.err = err.str();
        return run;
    }

    std::vector<std::string> lines(const std::string& text)
    {
        std::vector<std::string> result;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
            result.push_back(line);
        return result;
    }

    const char* const kScanR = R"({"op": "scan", "table": "R"})";
    const char* const kScanS = R"({"op": "scan", "table": "S"})";
    const char* const kScanT = R"({"op": "scan", "table": "T"})";

    // FROM R, S WHERE R.b = S.b (the comma form sql builds).
    std::string commaQuery()
    {
        return std::string(R"({"op": "select", "condition": "R.b = S.b", "child": {"op": "cross", "left": )") +
               kScanR + ", \"right\": " + kScanS + "}}";
    }

    // FROM R JOIN S ON R.b = S.b.
    std::string joinOnQuery()
    {
        return std::string(R"({"op": "join", "condition": "R.b = S.b", "left": )") + kScanR +
               ", \"right\": " + kScanS + "}";
    }

    // FROM R, T, S WHERE R.b = S.b AND S.c = T.c: written R, T, S, so the
    // first written pair (R, T) has no predicate between them.
    std::string threeTableQuery()
    {
        return std::string(R"({"op": "select", "condition": "R.b = S.b and S.c = T.c", "child": {"op": "cross", )") +
               R"("left": {"op": "cross", "left": )" + kScanR + ", \"right\": " + kScanT + "}, \"right\": " + kScanS +
               "}}";
    }

    // The seqscan tables of a physical plan JSON, in pre-order.
    void collectTables(const Json& node, std::vector<std::string>& tables)
    {
        if (node["op"] == "seqscan")
            tables.push_back(node["table"].get<std::string>());
        for (const char* input : {"child", "left", "right"})
            if (node.contains(input))
                collectTables(node[input], tables);
    }

    std::vector<std::string> tablesOf(const std::string& planJson)
    {
        std::vector<std::string> tables;
        collectTables(Json::parse(planJson), tables);
        return tables;
    }

    // The lines of `text` from the line `heading` up to the next blank line.
    std::vector<std::string> section(const std::string& text, const std::string& heading)
    {
        std::vector<std::string> result;
        bool inside = false;
        for (const std::string& line : lines(text))
        {
            if (line == heading)
                inside = true;
            else if (inside && line.empty())
                break;
            else if (inside)
                result.push_back(line);
        }
        return result;
    }

    // The position of the first line of `text` that starts (after its
    // indentation) with `label`, or text size when none does.
    std::size_t lineIndexOf(const std::vector<std::string>& text, const std::string& label)
    {
        for (std::size_t i = 0; i < text.size(); ++i)
            if (text[i].find_first_not_of(' ') != std::string::npos &&
                text[i].compare(text[i].find_first_not_of(' '), label.size(), label) == 0)
                return i;
        return text.size();
    }
} // namespace

TEST_CASE("opt analyze: a one-line summary; running it twice changes nothing (§M4.9)")
{
    TestDatabase db("analyze");
    const Run first = opt({"analyze", "--db", db.dir(), "--table", "R"});
    REQUIRE(first.code == 0);
    CHECK(first.err.empty());
    CHECK(first.out == "analyzed R: 200 rows, 1 pages, width 8\n");

    const Run second = opt({"analyze", "--db", db.dir(), "--table", "R", "--buckets", "20"});
    CHECK(second.code == 0);
    CHECK(second.out == first.out);

    const Run json = opt({"analyze", "--db", db.dir(), "--table", "R", "--buckets", "5", "--json"});
    REQUIRE(json.code == 0);
    CHECK(json.out == "{\"table\":\"R\",\"rows\":200,\"pages\":1,\"width\":8.0}\n");

    // The statistics were saved with --buckets 5: R.a holds 200 different
    // values, so its histogram has exactly 5 buckets of 40.
    auto engine = db::storage::StorageEngine::open(db.dir(), 8, db::storage::ReplacementPolicy::Lru);
    const std::optional<db::storage::TableInfo> info = engine->catalog().getTable("R");
    REQUIRE(info);
    REQUIRE(info->stats);
    CHECK(info->stats->columns[0].histogram.size() == 5);
    engine->close();
}

TEST_CASE("opt plan output runs in exec unedited, with the right rows (§M4.6)")
{
    TestDatabase db("plan_exec");
    const std::string query = db.file("q.json", commaQuery());
    for (const std::string frames : {"32", "4"})
    {
        CAPTURE(frames);
        const Run plan = opt({"plan", "--db", db.dir(), "--logical", query, "--frames", frames, "--json"});
        REQUIRE(plan.code == 0);
        CHECK(plan.err.empty());
        // Nothing but the plan on stdout: it parses as JSON as a whole.
        Json parsed;
        CHECK_NOTHROW(parsed = Json::parse(plan.out));

        const std::string planFile = db.file("p.json", plan.out);
        const Run run = exec({"run", "--db", db.dir(), "--plan", planFile, "--frames", frames, "--json"});
        REQUIRE(run.code == 0);
        CHECK(Json::parse(run.out)["rows"].size() == 200);
    }

    // The same after ANALYZE (other estimates, still a valid plan), and for
    // the three-table query.
    REQUIRE(opt({"analyze", "--db", db.dir(), "--table", "R"}).code == 0);
    REQUIRE(opt({"analyze", "--db", db.dir(), "--table", "S"}).code == 0);
    const Run plan = opt({"plan", "--db", db.dir(), "--logical", db.file("q3.json", threeTableQuery())});
    REQUIRE(plan.code == 0);
    const Run run = exec({"run", "--db", db.dir(), "--plan", db.file("p3.json", plan.out), "--json"});
    REQUIRE(run.code == 0);
    CHECK(Json::parse(run.out)["rows"].size() == 200);
}

TEST_CASE("opt explain: est_rows and est_cost on every line; --show-rewrite; --json")
{
    TestDatabase db("explain");
    const std::string query = db.file("q.json", threeTableQuery());

    const Run text = opt({"explain", "--db", db.dir(), "--logical", query, "--frames", "32"});
    REQUIRE(text.code == 0);
    const std::vector<std::string> physical = lines(text.out);
    REQUIRE(!physical.empty());
    for (const std::string& line : physical)
    {
        CAPTURE(line);
        CHECK(line.find("est_rows=") != std::string::npos);
        CHECK(line.find("est_cost=") != std::string::npos);
    }
    // Same files, same flags: same text.
    CHECK(opt({"explain", "--db", db.dir(), "--logical", query, "--frames", "32"}).out == text.out);

    // --json: one record per line of the text, in the same order.
    const Run json = opt({"explain", "--db", db.dir(), "--logical", query, "--frames", "32", "--json"});
    REQUIRE(json.code == 0);
    const Json records = Json::parse(json.out);
    REQUIRE(records.size() == physical.size());
    for (std::size_t i = 0; i < physical.size(); ++i)
    {
        CHECK(physical[i].find("est_rows=" + records[i]["est_rows"].dump()) != std::string::npos);
        CHECK(physical[i].find("est_cost=" + records[i]["est_cost"].dump()) != std::string::npos);
    }

    // --show-rewrite: before, after, physical.
    const Run rewrite = opt({"explain", "--db", db.dir(), "--logical", query, "--frames", "32", "--show-rewrite"});
    REQUIRE(rewrite.code == 0);
    const std::vector<std::string> before = section(rewrite.out, "Logical plan before rewrite:");
    const std::vector<std::string> after = section(rewrite.out, "Logical plan after rewrite:");
    CHECK(section(rewrite.out, "Physical plan:") == physical);
    REQUIRE(!before.empty());
    REQUIRE(!after.empty());
    CHECK(before[0].rfind("select (R.b = S.b and S.c = T.c)", 0) == 0);
    CHECK(lineIndexOf(after, "join") < after.size());
    for (const std::string& line : before)
        CHECK(line.find("est_cost=") == std::string::npos);
}

TEST_CASE("opt dp-table: one row per subset, the pair with no predicate skipped (§M4.5)")
{
    TestDatabase db("dp_table");
    const Run run = opt({"dp-table", "--db", db.dir(), "--logical", db.file("q.json", threeTableQuery())});
    REQUIRE(run.code == 0);
    const std::vector<std::string> out = lines(run.out);
    REQUIRE(out.size() >= 4);
    CHECK(out[0] == "Join block: R, T, S");
    CHECK(out[2] == "| Subset | Best plan considered | Cost |");
    std::size_t subsetRows = 0;
    for (const std::string& line : out)
        if (line.rfind("| {", 0) == 0)
            ++subsetRows;
    CHECK(subsetRows == 7); // 3 relations, 3 pairs, 1 triple
    CHECK(run.out.find("| {R,T} | no predicate, skipped | — |") != std::string::npos);

    // No join: no DP table, and it says so.
    const Run single = opt({"dp-table", "--db", db.dir(), "--logical", db.file("one.json", kScanR)});
    REQUIRE(single.code == 0);
    CHECK(single.out.rfind("No join block", 0) == 0);
}

TEST_CASE("opt errors: exit 1 for a bad query, exit 2 for a bad command line")
{
    TestDatabase db("errors");
    const Run unknown = opt({"plan", "--db", db.dir(), "--logical",
                             db.file("bad.json", R"({"op": "scan", "table": "Nope"})")});
    CHECK(unknown.code == 1);
    CHECK(unknown.err.rfind("error: name: ", 0) == 0);
    CHECK(unknown.out.empty());

    const Run missingFile = opt({"plan", "--db", db.dir(), "--logical", db.dir() + "/missing.json"});
    CHECK(missingFile.code == 1);

    const Run analyzeUnknown = opt({"analyze", "--db", db.dir(), "--table", "Nope"});
    CHECK(analyzeUnknown.code == 1);
    CHECK(analyzeUnknown.err.rfind("error: name: ", 0) == 0);

    const std::string query = db.file("q.json", commaQuery());
    const std::vector<std::vector<std::string>> usage = {
        {},
        {"optimize", "--db", db.dir()},
        {"plan", "--db", db.dir()},                                       // missing --logical
        {"plan", "--logical", query},                                     // missing --db
        {"analyze", "--db", db.dir()},                                    // missing --table
        {"analyze", "--db", db.dir(), "--table", "R", "--buckets", "0"},
        {"plan", "--db", db.dir(), "--logical", query, "--frames", "2"},
        {"plan", "--db", db.dir(), "--logical", query, "--frames", "lots"},
        {"plan", "--db", db.dir(), "--logical", query, "--color"},
        {"plan", "--db", db.dir(), "--logical"},                          // no value
        {"plan", "--db", db.dir(), "--logical", query, "--disable-rule", "8"},
        {"plan", "--db", db.dir(), "--logical", query, "--disable-rule", "0"},
        {"plan", "--db", db.dir(), "--logical", query, "--disable-rule", "2", "--unoptimized"},
        {"plan", "--db", db.dir(), "--logical", query, "--show-rewrite"},
        {"explain", "--db", db.dir(), "--logical", query, "--show-rewrite", "--json"},
        {"dp-table", "--db", db.dir(), "--logical", query, "--unoptimized"},
        {"analyze", "--db", db.dir(), "--table", "R", "--logical", query},
        {"plan", "--db", db.dir(), "--logical", query, "--table", "R"},
    };
    for (const std::vector<std::string>& args : usage)
    {
        CAPTURE(args.size());
        const Run run = opt(args);
        CHECK(run.code == 2);
        CHECK(run.err.rfind("usage error: ", 0) == 0);
        CHECK(run.err.find("opt explain") != std::string::npos);
        CHECK(run.out.empty());
    }
}

TEST_CASE("opt: --catalog is an alias of --db; comma join = JOIN ON (§M4.9)")
{
    TestDatabase db("alias");
    const std::string query = db.file("q.json", commaQuery());
    const Run withDb = opt({"plan", "--db", db.dir(), "--logical", query});
    const Run withCatalog = opt({"plan", "--catalog", db.dir(), "--logical", query});
    REQUIRE(withDb.code == 0);
    CHECK(withCatalog.code == 0);
    CHECK(withCatalog.out == withDb.out);

    const Run joinOn = opt({"plan", "--db", db.dir(), "--logical", db.file("on.json", joinOnQuery())});
    REQUIRE(joinOn.code == 0);
    CHECK(joinOn.out == withDb.out);
}

TEST_CASE("opt switches: --unoptimized keeps the written order; --disable-rule 2 keeps the select up")
{
    TestDatabase db("switches");
    const std::string three = db.file("q3.json", threeTableQuery());

    // Written R, T, S. Unoptimized keeps that order (R x T first); the
    // optimizer never starts with the pair that has no predicate.
    const Run unoptimized = opt({"plan", "--db", db.dir(), "--logical", three, "--unoptimized"});
    REQUIRE(unoptimized.code == 0);
    CHECK(tablesOf(unoptimized.out) == std::vector<std::string>{"R", "T", "S"});
    const Run optimized = opt({"plan", "--db", db.dir(), "--logical", three});
    REQUIRE(optimized.code == 0);
    CHECK(tablesOf(optimized.out) != std::vector<std::string>{"R", "T", "S"});

    // The unoptimized plan returns the same rows.
    const Run run = exec({"run", "--db", db.dir(), "--plan", db.file("p.json", unoptimized.out), "--json"});
    REQUIRE(run.code == 0);
    CHECK(Json::parse(run.out)["rows"].size() == 200);

    // R.a > 5 uses R only. With every rule on it goes below the join; with
    // rule 2 off it stays above it.
    const std::string filtered =
        db.file("f.json", std::string(R"({"op": "select", "condition": "R.a > 5 and R.b = S.b", "child": )") +
                              R"({"op": "cross", "left": )" + kScanR + ", \"right\": " + kScanS + "}}");
    const Run all = opt({"explain", "--db", db.dir(), "--logical", filtered, "--show-rewrite"});
    REQUIRE(all.code == 0);
    const std::vector<std::string> allAfter = section(all.out, "Logical plan after rewrite:");
    CHECK(lineIndexOf(allAfter, "select (R.a > 5)") > lineIndexOf(allAfter, "join (R.b = S.b)"));

    const Run off =
        opt({"explain", "--db", db.dir(), "--logical", filtered, "--show-rewrite", "--disable-rule", "2"});
    REQUIRE(off.code == 0);
    const std::vector<std::string> offAfter = section(off.out, "Logical plan after rewrite:");
    REQUIRE(lineIndexOf(offAfter, "select (R.a > 5)") < offAfter.size());
    CHECK(lineIndexOf(offAfter, "select (R.a > 5)") < lineIndexOf(offAfter, "join (R.b = S.b)"));
}
