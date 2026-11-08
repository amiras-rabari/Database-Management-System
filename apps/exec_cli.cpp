#include "exec_cli.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/counters.h"
#include "db/exec/exec_context.h"
#include "db/exec/executable_plan.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"

namespace db::apps
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        /// Frames when `--frames` is omitted (the same as the storage tool).
        constexpr std::size_t kDefaultFrames = 64;

        const char* const kUsage = "usage:\n"
                                   "  exec run   --db DIR --plan plan.json [--frames B] [--json]\n"
                                   "  exec trace --db DIR --plan plan.json [--frames B] [--json]\n"
                                   "(--catalog is an alias of --db; --frames defaults to 64)\n";

        /// A bad command line: exit code 2, message + usage on `err`.
        class UsageError : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

        struct Options
        {
            bool trace = false; // the command: trace, else run
            std::string db;
            std::string plan;
            std::size_t frames = kDefaultFrames;
            bool json = false;
        };

        // Reads the command line by hand: compare each argument with the
        // known flags (no regular expressions, §0.2).
        Options parseOptions(const std::vector<std::string>& args)
        {
            if (args.empty())
                throw UsageError("missing command");
            Options options;
            if (args[0] == "trace")
                options.trace = true;
            else if (args[0] != "run")
                throw UsageError("unknown command '" + args[0] + "'");

            bool hasDb = false;
            bool hasPlan = false;
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                const std::string& flag = args[i];
                if (flag == "--json")
                {
                    options.json = true;
                    continue;
                }
                if (flag != "--db" && flag != "--catalog" && flag != "--plan" && flag != "--frames")
                    throw UsageError("unknown option '" + flag + "'");
                if (i + 1 == args.size())
                    throw UsageError("option '" + flag + "' needs a value");
                const std::string& value = args[++i];
                if (flag == "--db" || flag == "--catalog")
                {
                    options.db = value;
                    hasDb = true;
                }
                else if (flag == "--plan")
                {
                    options.plan = value;
                    hasPlan = true;
                }
                else
                {
                    const std::optional<std::int32_t> n = parseInt32(value);
                    if (!n || *n < 1)
                        throw UsageError("--frames must be a whole number of at least 1, got '" + value + "'");
                    options.frames = static_cast<std::size_t>(*n);
                }
            }
            if (!hasDb)
                throw UsageError("missing --db");
            if (!hasPlan)
                throw UsageError("missing --plan");
            return options;
        }

        std::string readFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot read plan file '" + path + "'");
            std::ostringstream text;
            text << file.rdbuf();
            return text.str();
        }

        // --- formats -----------------------------------------------------

        /// [["R.a","INT"],["S.c","VARCHAR",20],["avg_R_salary","REAL"]] (§M2.5).
        Json schemaJson(const Schema& schema)
        {
            Json columns = Json::array();
            for (const Column& column : schema.columns())
            {
                Json entry = Json::array({column.qualifiedName(), std::string(typeName(column.type))});
                if (column.type == Type::Varchar)
                    entry.push_back(column.varcharLength);
                columns.push_back(entry);
            }
            return columns;
        }

        /// One value as JSON text. Written by hand so that a REAL keeps its
        /// four decimals (3.5000); a JSON library would write 3.5.
        std::string valueJson(const Value& value)
        {
            if (value.type() == Type::Varchar)
                return Json(value.asVarchar()).dump(); // quotes and escapes
            return value.toText(); // INT: 42; REAL: 3.5000
        }

        std::string rowJson(const Tuple& row)
        {
            std::string text = "[";
            for (std::size_t i = 0; i < row.size(); ++i)
                text += (i == 0 ? "" : ",") + valueJson(row[i]);
            return text + "]";
        }

        Json countersJson(const exec::ExecCounters& counters)
        {
            return Json{{"tuples_out", counters.tuplesOut},
                        {"comparisons", counters.comparisons},
                        {"page_reads", counters.pageReads},
                        {"page_writes", counters.pageWrites}};
        }

        std::string totalsText(const exec::ExecCounters& totals)
        {
            return "tuples_out=" + std::to_string(totals.tuplesOut) +
                   " comparisons=" + std::to_string(totals.comparisons) +
                   " reads=" + std::to_string(totals.pageReads) + " writes=" + std::to_string(totals.pageWrites);
        }

        /// `text` followed by spaces up to `width` characters, then 2 more.
        std::string pad(const std::string& text, std::size_t width)
        {
            return text + std::string(width - text.size() + 2, ' ');
        }

        /// §M2.5's layout: one line per node, indented by depth, columns lined up.
        /// hash_join          tuples_out=1     comparisons=4000  reads=96  writes=48
        ///   seqscan R        tuples_out=200   comparisons=0     reads=24  writes=0
        std::string traceText(const std::vector<exec::TraceRow>& rows)
        {
            // The cells of each line, then the widest cell of each column.
            std::vector<std::vector<std::string>> cells;
            for (const exec::TraceRow& row : rows)
                cells.push_back({std::string(2 * row.depth, ' ') + row.label,
                                 "tuples_out=" + std::to_string(row.counters.tuplesOut),
                                 "comparisons=" + std::to_string(row.counters.comparisons),
                                 "reads=" + std::to_string(row.subtree.pageReads),
                                 "writes=" + std::to_string(row.subtree.pageWrites)});
            std::vector<std::size_t> widths(5, 0);
            for (const std::vector<std::string>& line : cells)
                for (std::size_t c = 0; c < line.size(); ++c)
                    widths[c] = std::max(widths[c], line[c].size());

            std::string text;
            for (const std::vector<std::string>& line : cells)
            {
                for (std::size_t c = 0; c + 1 < line.size(); ++c)
                    text += pad(line[c], widths[c]);
                text += line.back() + "\n";
            }
            return text;
        }

        /// `maxFramesHeld`: the pool's peak of pinned + reserved frames during
        /// the run, the "largest number of frames held at once" of §M2.6
        /// measurement 5.
        Json traceJson(const std::vector<exec::TraceRow>& rows, const exec::ExecCounters& totals,
                       std::size_t maxFramesHeld)
        {
            Json nodes = Json::array();
            for (const exec::TraceRow& row : rows)
            {
                Json node = Json{{"depth", row.depth}, {"label", row.label}};
                const Json own = countersJson(row.counters);
                for (auto it = own.begin(); it != own.end(); ++it)
                    node[it.key()] = it.value();
                node["subtree"] = countersJson(row.subtree);
                node["details"] = row.details;
                nodes.push_back(node);
            }
            return Json{{"nodes", nodes}, {"totals", countersJson(totals)}, {"max_frames_held", maxFramesHeld}};
        }

        // --- the command -------------------------------------------------

        int runCommand(const Options& options, std::ostream& out)
        {
            // The plan is read and checked before the database is opened: a
            // malformed plan is rejected before any execution (§M2.2).
            const PlanNode plan = physicalFromJsonText(readFile(options.plan));
            std::unique_ptr<storage::StorageEngine> engine =
                storage::StorageEngine::open(std::filesystem::path(options.db), options.frames,
                                             storage::ReplacementPolicy::Lru);
            exec::ExecContext ctx(*engine);
            // Building checks every name and type before any page is read.
            exec::ExecutablePlan executable = exec::buildExecutablePlan(plan, ctx);

            executable.open();
            Tuple row;
            bool first = true;
            if (!options.trace)
            {
                // Rows go out one at a time as the plan produces them.
                if (options.json)
                    out << "{\"schema\":" << schemaJson(executable.schema()).dump() << ",\"rows\":[";
                else
                {
                    const std::vector<Column>& columns = executable.schema().columns();
                    for (std::size_t i = 0; i < columns.size(); ++i)
                        out << (i == 0 ? "" : " | ") << columns[i].qualifiedName();
                    out << "\n";
                }
            }
            while (executable.next(row))
            {
                if (options.trace)
                    continue; // trace prints counters only
                if (options.json)
                    out << (first ? "" : ",") << rowJson(row);
                else
                {
                    for (std::size_t i = 0; i < row.size(); ++i)
                        out << (i == 0 ? "" : " | ") << row[i].toText();
                    out << "\n";
                }
                first = false;
            }
            executable.close();

            const exec::ExecCounters totals = executable.totals();
            if (options.trace && options.json)
                out << traceJson(executable.trace(), totals, executable.maxFramesHeld()).dump() << "\n";
            else if (options.trace)
                out << traceText(executable.trace());
            else if (options.json)
                out << "],\"totals\":" << countersJson(totals).dump() << "}\n";
            else
                out << totalsText(totals) << "\n";

            engine->close();
            return 0;
        }
    } // namespace

    int runExecCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err)
    {
        // Every exception ends here; the engine (if any) was already
        // destroyed on the way out, which also deletes temporary files.
        try
        {
            return runCommand(parseOptions(args), out);
        }
        catch (const UsageError& e)
        {
            err << "usage error: " << e.what() << "\n" << kUsage;
            return 2;
        }
        catch (const Error& e)
        {
            err << formatError(e) << "\n";
            return 1;
        }
        catch (const std::logic_error& e)
        {
            err << "internal error: " << e.what() << "\n";
            return 3;
        }
        catch (const std::exception& e)
        {
            err << "error: " << e.what() << "\n";
            return 1;
        }
        catch (...)
        {
            err << "internal error: unknown exception\n";
            return 3;
        }
    }
} // namespace db::apps
