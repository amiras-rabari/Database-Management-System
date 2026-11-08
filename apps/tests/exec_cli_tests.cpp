// The `exec` CLI (exec chunk 14) through runExecCli with string streams:
// §M2.5's run --json object, the text output, the trace layout, REAL with
// four decimals, determinism, and every exit code.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/heap_file.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"
#include "exec_cli.h"

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::ordered_json;

    // A database in a fresh temp folder with one table
    // E(dept VARCHAR(5), salary INT) = ('cs', 10), ('ee', 21).
    class TestDatabase
    {
    public:
        explicit TestDatabase(const std::string& name)
            : dir_(fs::temp_directory_path() / "comp3005_exec_cli_tests" / name)
        {
            fs::remove_all(dir_);
            auto engine = db::storage::StorageEngine::open(dir_, 8, db::storage::ReplacementPolicy::Lru);
            const db::Schema schema({{"E", "dept", db::Type::Varchar, 5}, {"E", "salary", db::Type::Int, 0}});
            engine->catalog().createTable("E", schema);
            db::storage::HeapFile heap = engine->openHeapFile("E");
            heap.insert({db::Value::ofVarchar("cs"), db::Value::ofInt(10)});
            heap.insert({db::Value::ofVarchar("ee"), db::Value::ofInt(21)});
            engine->catalog().updateCounts("E", heap.numDataPages(), 2);
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

        /// Writes `json` to a plan file in the folder; returns its path.
        std::string plan(const std::string& json) const
        {
            const fs::path path = dir_ / "plan.json";
            std::ofstream(path) << json;
            return path.string();
        }

    private:
        fs::path dir_;
    };

    struct Run
    {
        int code = 0;
        std::string out;
        std::string err;
    };

    Run exec(const std::vector<std::string>& args)
    {
        std::ostringstream out, err;
        Run run;
        run.code = db::apps::runExecCli(args, out, err);
        run.out = out.str();
        run.err = err.str();
        return run;
    }

    const char* const kFilterPlan =
        R"({"op": "filter", "condition": "E.salary > 5", "child": {"op": "seqscan", "table": "E"}})";

    std::vector<std::string> lines(const std::string& text)
    {
        std::vector<std::string> result;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
            result.push_back(line);
        return result;
    }
} // namespace

TEST_CASE("exec run --json prints §M2.5's object: schema, rows, totals, in that order")
{
    TestDatabase db("run_json");
    const Run run = exec({"run", "--db", db.dir(), "--plan", db.plan(kFilterPlan), "--frames", "4", "--json"});
    REQUIRE(run.code == 0);
    CHECK(run.err.empty());
    const Json json = Json::parse(run.out);
    std::vector<std::string> keys;
    for (auto it = json.begin(); it != json.end(); ++it)
        keys.push_back(it.key());
    CHECK(keys == std::vector<std::string>{"schema", "rows", "totals"});
    CHECK(json["schema"].dump() == R"([["E.dept","VARCHAR",5],["E.salary","INT"]])");
    CHECK(json["rows"].dump() == R"([["cs",10],["ee",21]])");
    CHECK(json["rows"].size() == json["totals"]["tuples_out"].get<std::size_t>());
    CHECK(json["totals"]["comparisons"] == 2); // the filter looked at 2 tuples
    std::vector<std::string> totalKeys;
    for (auto it = json["totals"].begin(); it != json["totals"].end(); ++it)
        totalKeys.push_back(it.key());
    CHECK(totalKeys == std::vector<std::string>{"tuples_out", "comparisons", "page_reads", "page_writes"});
}

TEST_CASE("exec run without --json: names, rows, totals")
{
    TestDatabase db("run_text");
    const Run run = exec({"run", "--db", db.dir(), "--plan", db.plan(kFilterPlan)});
    REQUIRE(run.code == 0);
    const std::vector<std::string> out = lines(run.out);
    REQUIRE(out.size() == 4);
    CHECK(out[0] == "E.dept | E.salary");
    CHECK(out[1] == "cs | 10");
    CHECK(out[2] == "ee | 21");
    CHECK(out[3].rfind("tuples_out=2 comparisons=2 reads=", 0) == 0);
}

TEST_CASE("exec trace prints one line per node, indented by depth, columns lined up")
{
    TestDatabase db("trace_text");
    const Run run = exec({"trace", "--db", db.dir(), "--plan", db.plan(kFilterPlan), "--frames", "4"});
    REQUIRE(run.code == 0);
    const std::vector<std::string> out = lines(run.out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].rfind("filter ", 0) == 0);
    CHECK(out[1].rfind("  seqscan E ", 0) == 0);
    for (const char* key : {"tuples_out=", "comparisons=", "reads=", "writes="})
    {
        CAPTURE(key);
        CHECK(out[0].find(key) != std::string::npos);
        CHECK(out[0].find(key) == out[1].find(key)); // the same column on every line
    }
    CHECK(out[0].find("reads=") < out[0].find("writes="));
    CHECK(out[0].find("tuples_out=2 ") != std::string::npos);
    CHECK(out[1].find("comparisons=0 ") != std::string::npos);
}

TEST_CASE("exec trace --json gives every node's counters and a sort's passes")
{
    TestDatabase db("trace_json");
    const std::string plan =
        R"({"op": "sort", "keys": [["E.salary", "desc"]], "child": {"op": "seqscan", "table": "E"}})";
    const Run run = exec({"trace", "--db", db.dir(), "--plan", db.plan(plan), "--frames", "4", "--json"});
    REQUIRE(run.code == 0);
    const Json json = Json::parse(run.out);
    REQUIRE(json["nodes"].size() == 2);
    CHECK(json["nodes"][0]["label"] == "sort");
    CHECK(json["nodes"][0]["details"]["passes"] == 1);
    CHECK(json["nodes"][1]["label"] == "seqscan E");
    CHECK(json["nodes"][1]["depth"] == 1);
    CHECK(json["totals"]["tuples_out"] == 2);
    // A root sort of 1 page: the scan page + the sort's 1-page run, at most B = 4.
    CHECK(json["max_frames_held"].get<std::size_t>() >= 1);
    CHECK(json["max_frames_held"].get<std::size_t>() <= 4);
}

TEST_CASE("exec run writes AVG (REAL) with four decimals")
{
    TestDatabase db("run_real");
    const std::string plan = R"({"op": "group_by", "mode": "hash", "keys": [], "aggregates": [["avg", "E.salary"]],)"
                             R"( "child": {"op": "seqscan", "table": "E"}})";
    const Run text = exec({"run", "--db", db.dir(), "--plan", db.plan(plan)});
    REQUIRE(text.code == 0);
    CHECK(lines(text.out)[1] == "15.5000");
    const Run json = exec({"run", "--db", db.dir(), "--plan", db.plan(plan), "--json"});
    REQUIRE(json.code == 0);
    CHECK(json.out.find(R"("rows":[[15.5000]])") != std::string::npos);
    CHECK(Json::parse(json.out)["schema"].dump() == R"([["avg_E_salary","REAL"]])");
}

TEST_CASE("exec: the same command twice gives byte-identical output")
{
    TestDatabase db("determinism");
    const std::vector<std::string> args = {"trace", "--db", db.dir(), "--plan", db.plan(kFilterPlan), "--json"};
    CHECK(exec(args).out == exec(args).out);
}

TEST_CASE("exec: plan and database errors exit 1 with a message naming the problem")
{
    TestDatabase db("errors");
    const Run missing = exec({"run", "--db", db.dir(), "--plan",
                              db.plan(R"({"op": "sort", "child": {"op": "seqscan", "table": "E"}})")});
    CHECK(missing.code == 1);
    CHECK(missing.err.find("Node 'sort': missing required field 'keys'.") != std::string::npos);
    CHECK(missing.out.empty());

    const Run unknown =
        exec({"run", "--db", db.dir(), "--plan", db.plan(R"({"op": "seqscan", "table": "Nope"})")});
    CHECK(unknown.code == 1);
    CHECK(unknown.err.find("Nope") != std::string::npos);

    const std::string sort =
        R"({"op": "sort", "keys": [["E.salary", "asc"]], "child": {"op": "seqscan", "table": "E"}})";
    const Run tooSmall = exec({"run", "--db", db.dir(), "--plan", db.plan(sort), "--frames", "2"});
    CHECK(tooSmall.code == 1);
    CHECK(tooSmall.err.find("needs at least 3 frames") != std::string::npos);

    const Run noFile = exec({"run", "--db", db.dir(), "--plan", db.dir() + "/missing.json"});
    CHECK(noFile.code == 1);
}

TEST_CASE("exec: bad command lines exit 2 with a usage message")
{
    TestDatabase db("usage");
    const std::string plan = db.plan(kFilterPlan);
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {},
             {"explain", "--db", db.dir(), "--plan", plan},
             {"run", "--db", db.dir(), "--plan", plan, "--fast"},
             {"run", "--db", db.dir(), "--plan", plan, "--frames", "0"},
             {"run", "--db", db.dir(), "--plan", plan, "--frames", "x"},
             {"run", "--db", db.dir()},
             {"run", "--plan", plan},
             {"run", "--db", db.dir(), "--plan"}})
    {
        const Run run = exec(args);
        CHECK(run.code == 2);
        CHECK(run.err.find("usage:") != std::string::npos);
    }
    // --catalog is an alias of --db.
    CHECK(exec({"run", "--catalog", db.dir(), "--plan", plan}).code == 0);
}
