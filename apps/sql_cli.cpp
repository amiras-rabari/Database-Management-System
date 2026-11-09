// The `sql` tool (§M3.7): reads the command line, runs the parser and prints
// the result. See sql_cli.h for the commands and exit codes.

#include "sql_cli.h"

#include <cstddef>
#include <exception>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/sql/ast.h"
#include "db/sql/explain_ast.h"
#include "db/sql/parser.h"

namespace db::apps
{
    namespace
    {
        const char* const kUsage = "usage:\n"
                                   "  sql parse --query \"SQL\" [--explain]\n";

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
        catch (const std::exception& e)
        {
            // Nothing in `parse` should get here; print one line, never a
            // stack trace (§M3.6).
            err << "internal error: " << e.what() << "\n";
            return 3;
        }
    }
} // namespace db::apps
