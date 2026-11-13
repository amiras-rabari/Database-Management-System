#include "db/shell/shell.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "db/common/error.h"
#include "db/common/value.h"
#include "db/sql/lexer.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"
#include "statements.h"

namespace db::shell
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        /// Frames when `--frames` is omitted (the same as the other tools).
        constexpr std::int32_t kDefaultFrames = 64;

        const char* const kUsage =
            "usage: shell --db DIR [--frames B] [--file F] [--json]\n"
            "(--catalog is an alias of --db; --frames defaults to 64 and must be at least 3)\n";

        /// A bad command line: exit code 2, message + usage on `err`.
        class UsageError : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

        struct Options
        {
            std::string db;
            std::int32_t frames = kDefaultFrames;
            std::string file; // empty = read from `in`, with a prompt
            bool json = false;
        };

        // Reads the command line by hand, like the other four tools.
        Options parseOptions(const std::vector<std::string>& args)
        {
            Options options;
            bool hasDb = false;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string& flag = args[i];
                if (flag == "--json")
                {
                    options.json = true;
                    continue;
                }
                if (flag != "--db" && flag != "--catalog" && flag != "--frames" && flag != "--file")
                    throw UsageError("unknown option '" + flag + "'");
                if (i + 1 == args.size())
                    throw UsageError("option '" + flag + "' needs a value");
                const std::string& value = args[++i];
                if (flag == "--db" || flag == "--catalog")
                {
                    options.db = value;
                    hasDb = true;
                }
                else if (flag == "--file")
                    options.file = value;
                else
                {
                    // At least 3: opt's cost model divides by B - 2 and
                    // ANALYZE's external sort needs 3 frames (shell STATUS).
                    const std::optional<std::int32_t> frames = parseInt32(value);
                    if (!frames || *frames < 3)
                        throw UsageError("--frames must be a whole number of at least 3, got '" + value + "'");
                    options.frames = *frames;
                }
            }
            if (!hasDb)
                throw UsageError("missing --db");
            return options;
        }

        // True when `line` ends with ';' after any trailing spaces, tabs or
        // '\r' (a Windows line ending): the end of a piece.
        bool endsWithSemicolon(const std::string& line)
        {
            const std::size_t last = line.find_last_not_of(" \t\r");
            return last != std::string::npos && line[last] == ';';
        }

        bool isBlank(const std::string& line)
        {
            return line.find_first_not_of(" \t\r") == std::string::npos;
        }

        // Prints a failed statement's message on err (and its --json line)
        // and counts it. A failure never stops the script (psql's default).
        void fail(Session& session, const std::string& message)
        {
            ++session.failures;
            session.err << message << "\n";
            if (session.json)
                session.out << Json{{"statement", session.statementNumber}, {"ok", false}, {"error", message}}.dump()
                            << "\n";
        }

        // The --json line of a statement that worked: numbers only. A query
        // adds the four numbers of the hidden-workload report and one entry
        // per plan node, in pre-order (estimate i and trace row i are the
        // same node). Field names are defined only here.
        Json jsonLine(int statementNumber, const std::optional<QueryRun>& run)
        {
            Json line{{"statement", statementNumber}, {"ok", true}};
            if (!run)
                return line;
            line["rows"] = run->totals.tuplesOut;
            line["page_reads"] = run->totals.pageReads;
            line["page_writes"] = run->totals.pageWrites;
            line["wall_ms"] = run->wallMs;
            line["est_root_rows"] = run->optimized.estimates[0].rows;
            line["actual_root_rows"] = run->totals.tuplesOut;
            Json operators = Json::array();
            for (std::size_t i = 0; i < run->trace.size(); ++i)
                operators.push_back(Json{{"label", run->trace[i].label},
                                         {"est_rows", run->optimized.estimates[i].rows},
                                         {"tuples_out", run->trace[i].counters.tuplesOut}});
            line["operators"] = operators;
            return line;
        }

        // Runs one statement and prints its --json line. A db::Error or a
        // storage file error fails only this statement; a std::logic_error
        // (a bug) goes up and ends the shell with exit code 3.
        void runOne(Session& session, const std::string& piece, const std::vector<sql::SqlToken>& tokens,
                    std::size_t first, std::size_t end, int firstLine)
        {
            ++session.statementNumber;
            try
            {
                const std::optional<QueryRun> run = runStatement(session, piece, tokens, first, end);
                // A crash between statements then loses nothing.
                session.engine.flushAll();
                if (session.json)
                    session.out << jsonLine(session.statementNumber, run).dump() << "\n";
            }
            catch (const Error& error)
            {
                // The position inside the piece becomes a position in the
                // whole script (a piece starts at the start of a line).
                fail(session, formatError(movedError(error, firstLine, 1)));
            }
            catch (const std::runtime_error& error)
            {
                fail(session, std::string("error: ") + error.what());
            }
        }

        // Runs one piece: whole lines of the input, ending with a line that
        // ends with ';' (or at the end of the input). `firstLine` is the
        // piece's first line in the whole input.
        //
        // sql's tokenizer finds the statements: every ';' token ends one, so
        // a ';' inside a string or a `--` comment does not split. Each
        // statement is compiled and run before the next one is compiled, so
        // a table created by one statement is known to the next.
        void runPiece(Session& session, const std::string& piece, int firstLine)
        {
            std::vector<sql::SqlToken> tokens;
            try
            {
                tokens = sql::tokenizeSql(piece);
            }
            catch (const Error& error)
            {
                // A lexical error (an unterminated string, a bad character)
                // fails this piece only; the next lines still run.
                ++session.statementNumber;
                fail(session, formatError(movedError(error, firstLine, 1)));
                return;
            }
            std::size_t first = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const sql::SqlTokenKind kind = tokens[i].kind;
                if (kind != sql::SqlTokenKind::Semicolon && kind != sql::SqlTokenKind::EndOfInput)
                    continue;
                if (i > first) // empty statements are skipped
                    runOne(session, piece, tokens, first, i, firstLine);
                first = i + 1;
            }
        }

        // Reads `input` line by line and runs each piece. With `prompt`,
        // writes `db> ` before the first line of a statement and `...> `
        // before a line that continues one, on err, so redirected results
        // hold only result lines.
        void readScript(Session& session, std::istream& input, bool prompt)
        {
            std::string piece;
            int pieceFirstLine = 1;
            int lineNumber = 0;
            std::string line;
            if (prompt)
                session.err << "db> ";
            while (std::getline(input, line))
            {
                ++lineNumber;
                if (piece.empty() && isBlank(line))
                {
                    if (prompt)
                        session.err << "db> ";
                    continue;
                }
                if (piece.empty())
                    pieceFirstLine = lineNumber;
                piece += line;
                piece += '\n';
                if (endsWithSemicolon(line))
                {
                    runPiece(session, piece, pieceFirstLine);
                    piece.clear();
                }
                if (prompt)
                    session.err << (piece.empty() ? "db> " : "...> ");
            }
            // The last statement may have no ';'.
            if (!piece.empty())
                runPiece(session, piece, pieceFirstLine);
        }

        int run(const Options& options, std::istream& in, std::ostream& out, std::ostream& err)
        {
            std::ifstream file;
            if (!options.file.empty())
            {
                file.open(options.file, std::ios::binary);
                if (!file)
                {
                    err << "error: cannot read SQL file '" << options.file << "'\n";
                    return 1;
                }
            }
            // One engine for the whole run: every statement uses its pool.
            std::unique_ptr<storage::StorageEngine> engine = storage::StorageEngine::open(
                std::filesystem::path(options.db), static_cast<std::size_t>(options.frames),
                storage::ReplacementPolicy::Lru);
            Session session{*engine, options.frames, options.json, out, err};
            readScript(session, options.file.empty() ? in : file, options.file.empty());
            // Closed here, not by the destructor, so a failed save is seen.
            engine->close();
            return session.failures == 0 ? 0 : 1;
        }
    } // namespace

    int runShell(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err)
    {
        try
        {
            return run(parseOptions(args), in, out, err);
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
} // namespace db::shell
