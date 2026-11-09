// The `sql` tool (§M3.7): reads the command line, runs the parser or the
// compiler and prints the result. See sql_cli.h for the commands and exit
// codes.

#include "sql_cli.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/sql/ast.h"
#include "db/sql/compile.h"
#include "db/sql/explain_ast.h"
#include "db/sql/parser.h"
#include "db/sql/table_lookup.h"
#include "db/storage/catalog.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"

namespace db::apps
{
    namespace
    {
        /// Frames when `--frames` is omitted (the same as the storage and
        /// exec tools). Here the pool only reads the catalog.
        constexpr std::size_t kDefaultFrames = 64;

        const char* const kUsage = "usage:\n"
                                   "  sql parse --query \"SQL\" [--explain]\n"
                                   "  sql compile --db DIR --file q.sql [--json] [--frames B]\n"
                                   "  sql check --db DIR --file q.sql [--json] [--frames B]\n"
                                   "(--catalog is an alias of --db; --frames defaults to 64)\n";

        /// A bad command line: exit code 2, message + usage on `err`.
        class UsageError : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

        struct ParseOptions
        {
            std::optional<std::string> query;
            bool explain = false;
        };

        // Reads `parse`'s flags by hand: compare each argument with the known
        // flags (no regular expressions, §0.2). args[0] is the command.
        ParseOptions readParseOptions(const std::vector<std::string>& args)
        {
            ParseOptions options;
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                if (args[i] == "--query")
                {
                    if (i + 1 >= args.size())
                    {
                        throw UsageError("--query needs the SQL text after it");
                    }
                    options.query = args[++i];
                }
                else if (args[i] == "--explain")
                {
                    options.explain = true;
                }
                else
                {
                    throw UsageError("unknown option '" + args[i] + "'");
                }
            }
            if (!options.query)
            {
                throw UsageError("parse needs --query \"SQL\"");
            }
            return options;
        }

        // `sql parse`: the whole text is parsed before anything is printed,
        // so an error in a later statement prints no half output.
        int runParse(const std::vector<std::string>& args, std::ostream& out)
        {
            const ParseOptions options = readParseOptions(args);
            const std::vector<sql::Statement> statements = sql::parseSqlScript(*options.query);
            if (options.explain)
            {
                for (const sql::Statement& statement : statements)
                {
                    out << sql::explainAst(statement);
                }
            }
            return 0;
        }

        struct CompileOptions
        {
            std::string db;
            std::string file;
            std::size_t frames = kDefaultFrames;
        };

        // Reads the flags of `compile` and `check` (the same for both).
        // `--json` is accepted and changes nothing: compile's only output is
        // JSON, and check prints only errors (G3: every tool takes --json).
        CompileOptions readCompileOptions(const std::vector<std::string>& args)
        {
            CompileOptions options;
            bool hasDb = false;
            bool hasFile = false;
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                const std::string& flag = args[i];
                if (flag == "--json")
                {
                    continue;
                }
                if (flag != "--db" && flag != "--catalog" && flag != "--file" && flag != "--frames")
                {
                    throw UsageError("unknown option '" + flag + "'");
                }
                if (i + 1 == args.size())
                {
                    throw UsageError("option '" + flag + "' needs a value");
                }
                const std::string& value = args[++i];
                if (flag == "--db" || flag == "--catalog")
                {
                    options.db = value;
                    hasDb = true;
                }
                else if (flag == "--file")
                {
                    options.file = value;
                    hasFile = true;
                }
                else
                {
                    const std::optional<std::int32_t> n = parseInt32(value);
                    if (!n || *n < 1)
                    {
                        throw UsageError("--frames must be a whole number of at least 1, got '" + value + "'");
                    }
                    options.frames = static_cast<std::size_t>(*n);
                }
            }
            if (!hasDb)
            {
                throw UsageError("missing --db");
            }
            if (!hasFile)
            {
                throw UsageError("missing --file");
            }
            return options;
        }

        // The whole SQL file as text (apps may read files; the library may not).
        std::string readFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                throw std::runtime_error("cannot read SQL file '" + path + "'");
            }
            std::ostringstream text;
            text << file.rdbuf();
            return text.str();
        }

        // Compiles the file against the catalog of `--db`. This is the one
        // place the sql tool meets storage: the lambda turns the catalog into
        // sql's TableLookup, so the db_sql library never links storage. The
        // engine lives until the end of this function (its destructor
        // closes it, also after an error), and the lambda does not outlive it.
        // A missing folder is created with an empty catalog, so every table
        // is then unknown. Nothing is executed: a CREATE in the file is not
        // seen by a later statement.
        std::vector<sql::CompiledStatement> compileFile(const CompileOptions& options)
        {
            const std::string text = readFile(options.file);
            std::unique_ptr<storage::StorageEngine> engine = storage::StorageEngine::open(
                std::filesystem::path(options.db), options.frames, storage::ReplacementPolicy::Lru);
            const storage::Catalog& catalog = engine->catalog();
            const sql::TableLookup lookup = [&catalog](const std::string& name) -> std::optional<Schema>
            {
                const std::optional<storage::TableInfo> table = catalog.getTable(name);
                if (!table)
                {
                    return std::nullopt;
                }
                return table->schema;
            };
            return sql::compileSql(text, lookup);
        }

        // `sql compile`: prints the logical plan JSON (§M3.7) through common's
        // writer, the only place plan field names are defined. One query: its
        // plan; several: an array of the plans in order; CREATE and INSERT
        // print nothing. An SQL error goes to stderr (runSqlCli's handler).
        int runCompile(const std::vector<std::string>& args, std::ostream& out)
        {
            const std::vector<sql::CompiledStatement> statements = compileFile(readCompileOptions(args));
            std::vector<const PlanNode*> plans;
            for (const sql::CompiledStatement& statement : statements)
            {
                if (const auto* plan = std::get_if<PlanNode>(&statement))
                {
                    plans.push_back(plan);
                }
            }
            if (plans.size() == 1)
            {
                out << toJsonText(*plans[0]) << "\n";
            }
            else if (plans.size() > 1)
            {
                PlanJson array = PlanJson::array();
                for (const PlanNode* plan : plans)
                {
                    array.push_back(toJson(*plan));
                }
                out << array.dump(2) << "\n";
            }
            return 0;
        }

        // `sql check`: errors only (§M3.7). The SQL error is the command's
        // output, so it goes to `out`; exit 0 when there is none, 1 when
        // there is. A bad command line or an unreadable file is handled by
        // runSqlCli as for every command (exit 2, and 1 on stderr).
        int runCheck(const std::vector<std::string>& args, std::ostream& out)
        {
            const CompileOptions options = readCompileOptions(args);
            try
            {
                compileFile(options);
            }
            catch (const Error& e)
            {
                out << formatError(e) << "\n";
                return 1;
            }
            return 0;
        }
    } // namespace

    int runSqlCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err)
    {
        try
        {
            if (args.empty())
            {
                throw UsageError("missing command");
            }
            if (args[0] == "parse")
            {
                return runParse(args, out);
            }
            if (args[0] == "compile")
            {
                return runCompile(args, out);
            }
            if (args[0] == "check")
            {
                return runCheck(args, out);
            }
            throw UsageError("unknown command '" + args[0] + "'");
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
        // The same codes as the storage and exec tools: a bug is 3, any other
        // failure (e.g. a file that cannot be read) is 1. One line each,
        // never a stack trace (§M3.6).
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
