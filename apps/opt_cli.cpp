#include "opt_cli.h"

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
#include "db/common/value.h"
#include "db/opt/analyze.h"
#include "db/opt/defaults.h"
#include "db/opt/enumerate.h"
#include "db/opt/estimate.h"
#include "db/opt/explain.h"
#include "db/opt/optimizer.h"
#include "db/opt/rewrite.h"
#include "db/storage/catalog.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::apps
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        /// Frames when `--frames` is omitted (the same as the storage and
        /// exec tools).
        constexpr std::int32_t kDefaultFrames = 64;

        const char* const kUsage =
            "usage:\n"
            "  opt analyze  --db DIR --table R [--buckets 20] [--frames B] [--json]\n"
            "  opt plan     --db DIR --logical q.json [--frames B] [--json] [--disable-rule N]... [--unoptimized]\n"
            "  opt explain  --db DIR --logical q.json [--frames B] [--json] [--disable-rule N]... [--unoptimized]\n"
            "               [--show-rewrite]\n"
            "  opt dp-table --db DIR --logical q.json [--frames B] [--json] [--disable-rule N]...\n"
            "(--catalog is an alias of --db; --frames defaults to 64 and must be at least 3)\n";

        /// A bad command line: exit code 2, message + usage on `err`.
        class UsageError : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

        struct Options
        {
            std::string command; // analyze, plan, explain or dp-table
            std::string db;
            std::string table;   // analyze
            std::string logical; // the other three
            std::int32_t frames = kDefaultFrames;
            std::int32_t buckets = opt::kDefaultBuckets;
            bool json = false;
            opt::RuleSet rules = opt::kAllRules;
            bool unoptimized = false;
            bool showRewrite = false;
        };

        // A whole number of at least `minimum` for option `flag`, else a
        // usage error naming the option.
        std::int32_t parseNumber(const std::string& flag, const std::string& value, std::int32_t minimum)
        {
            const std::optional<std::int32_t> n = parseInt32(value);
            if (!n || *n < minimum)
                throw UsageError(flag + " must be a whole number of at least " + std::to_string(minimum) + ", got '" +
                                 value + "'");
            return *n;
        }

        // Reads the command line by hand: compare each argument with the
        // known flags (no regular expressions, §0.2). A flag that belongs to
        // another command is a usage error, never silently ignored.
        Options parseOptions(const std::vector<std::string>& args)
        {
            if (args.empty())
                throw UsageError("missing command");
            Options options;
            options.command = args[0];
            const bool analyze = options.command == "analyze";
            if (!analyze && options.command != "plan" && options.command != "explain" &&
                options.command != "dp-table")
                throw UsageError("unknown command '" + options.command + "'");

            bool hasDb = false;
            bool hasRuleSwitch = false;
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                const std::string& flag = args[i];

                // Which command each flag belongs to.
                if ((flag == "--table" || flag == "--buckets") && !analyze)
                    throw UsageError("option '" + flag + "' is only for analyze");
                if ((flag == "--logical" || flag == "--disable-rule" || flag == "--unoptimized") && analyze)
                    throw UsageError("option '" + flag + "' is not for analyze");
                if (flag == "--unoptimized" && options.command == "dp-table")
                    throw UsageError("dp-table has no unoptimized form (there is no DP)");
                if (flag == "--show-rewrite" && options.command != "explain")
                    throw UsageError("option '--show-rewrite' is only for explain");

                // Flags without a value.
                if (flag == "--json")
                {
                    options.json = true;
                    continue;
                }
                if (flag == "--unoptimized")
                {
                    options.unoptimized = true;
                    continue;
                }
                if (flag == "--show-rewrite")
                {
                    options.showRewrite = true;
                    continue;
                }

                // Flags with a value.
                if (flag != "--db" && flag != "--catalog" && flag != "--table" && flag != "--logical" &&
                    flag != "--frames" && flag != "--buckets" && flag != "--disable-rule")
                    throw UsageError("unknown option '" + flag + "'");
                if (i + 1 == args.size())
                    throw UsageError("option '" + flag + "' needs a value");
                const std::string& value = args[++i];
                if (flag == "--db" || flag == "--catalog")
                {
                    options.db = value;
                    hasDb = true;
                }
                else if (flag == "--table")
                    options.table = value;
                else if (flag == "--logical")
                    options.logical = value;
                else if (flag == "--frames")
                    options.frames = parseNumber(flag, value, 3);
                else if (flag == "--buckets")
                    options.buckets = parseNumber(flag, value, 1);
                else
                {
                    const std::int32_t rule = parseNumber(flag, value, 1);
                    if (rule > 7)
                        throw UsageError("--disable-rule must be a rule number from 1 to 7, got '" + value + "'");
                    options.rules[static_cast<std::size_t>(rule - 1)] = false;
                    hasRuleSwitch = true;
                }
            }
            if (!hasDb)
                throw UsageError("missing --db");
            if (analyze && options.table.empty())
                throw UsageError("analyze needs --table");
            if (!analyze && options.logical.empty())
                throw UsageError(options.command + " needs --logical");
            if (options.unoptimized && hasRuleSwitch)
                throw UsageError("--disable-rule cannot be used with --unoptimized (it sets the rules itself)");
            if (options.showRewrite && options.json)
                throw UsageError("--show-rewrite prints text; use it without --json");
            return options;
        }

        std::string readFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot read logical plan file '" + path + "'");
            std::ostringstream text;
            text << file.rdbuf();
            return text.str();
        }

        // The width for the summary line, to 6 significant digits: `40.96`,
        // the same on every run.
        std::string widthText(double width)
        {
            std::ostringstream text;
            text << width;
            return text.str();
        }

        int runAnalyze(const Options& options, storage::StorageEngine& engine, std::ostream& out)
        {
            const storage::TableStats stats = opt::analyzeTable(engine, options.table, options.buckets);
            // The statistics reach the catalog file only here (or at a
            // flush): close before reporting success, so a failed save is an
            // error, not a quiet loss.
            engine.close();
            if (options.json)
                out << Json{{"table", options.table},
                            {"rows", stats.nTuples},
                            {"pages", stats.nPages},
                            {"width", stats.width}}
                           .dump()
                    << "\n";
            else
                out << "analyzed " << options.table << ": " << stats.nTuples << " rows, " << stats.nPages
                    << " pages, width " << widthText(stats.width) << "\n";
            return 0;
        }

        // explain --json: one {est_rows, est_cost} per physical node, in
        // pre-order (index i = exec trace node i).
        std::string estimatesJson(const std::vector<opt::NodeEstimate>& estimates)
        {
            Json nodes = Json::array();
            for (const opt::NodeEstimate& estimate : estimates)
                nodes.push_back(Json{{"est_rows", estimate.rows}, {"est_cost", estimate.cost}});
            return nodes.dump() + "\n";
        }

        int runPlanCommand(const Options& options, const PlanNode& logical, storage::StorageEngine& engine,
                           std::ostream& out)
        {
            // The optimizer reads each table's catalog entry (rows, pages,
            // statistics) through this function; nothing else of the
            // database is read.
            const opt::StatsProvider stats = [&engine](const std::string& name) {
                return engine.catalog().getTable(name);
            };
            opt::OptimizerOptions optimizerOptions;
            optimizerOptions.frames = options.frames;
            optimizerOptions.rules = options.rules;
            optimizerOptions.unoptimized = options.unoptimized;
            const opt::OptimizedPlan optimized = opt::optimize(logical, stats, optimizerOptions);

            std::string text;
            if (options.command == "plan")
                text = toJsonText(optimized.physical) + "\n";
            else if (options.command == "dp-table")
            {
                for (std::size_t i = 0; i < optimized.dpTables.size(); ++i)
                    text += (i == 0 ? "" : "\n") + opt::dpTableText(optimized.dpTables[i]);
                if (optimized.dpTables.empty())
                    text = "No join block: the query joins fewer than two relations, so there is no DP table.\n";
            }
            else if (options.json)
                text = estimatesJson(optimized.estimates);
            else if (options.showRewrite)
            {
                // §M4.3: both logical trees (rows only: a logical join has no
                // algorithm, so no cost yet), then the physical plan.
                text = "Logical plan before rewrite:\n" +
                       opt::explainText(optimized.logicalBefore,
                                        opt::estimatePlan(optimized.logicalBefore, stats), false) +
                       "\nLogical plan after rewrite:\n" +
                       opt::explainText(optimized.logicalAfter, opt::estimatePlan(optimized.logicalAfter, stats),
                                        false) +
                       "\nPhysical plan:\n" + opt::explainText(optimized.physical, optimized.estimates, true);
            }
            else
                text = opt::explainText(optimized.physical, optimized.estimates, true);

            engine.close();
            out << text;
            return 0;
        }

        int runCommand(const Options& options, std::ostream& out)
        {
            // The logical plan is read and checked before the database is
            // opened: a malformed file fails before any work.
            PlanNode logical;
            if (options.command != "analyze")
                logical = fromJsonText(readFile(options.logical));

            std::unique_ptr<storage::StorageEngine> engine =
                storage::StorageEngine::open(std::filesystem::path(options.db),
                                             static_cast<std::size_t>(options.frames),
                                             storage::ReplacementPolicy::Lru);
            if (options.command == "analyze")
                return runAnalyze(options, *engine, out);
            return runPlanCommand(options, logical, *engine, out);
        }
    } // namespace

    int runOptCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err)
    {
        // Every exception ends here; the engine (if any) was already
        // destroyed on the way out, which releases its pages and deletes
        // temporary files.
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
