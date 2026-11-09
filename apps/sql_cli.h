#pragma once

/// @file
/// The `sql` command-line tool (SPEC §M3.7, sql chunk 05) as a function, so
/// tests can run it with string streams. `sql_main.cpp` only calls it with
/// std::cout / std::cerr. All printing is here, in apps/, never in the
/// library.
///
/// Commands:
/// ```
/// sql parse   --query "SQL" [--explain]
/// sql compile --db DIR --file q.sql [--json] [--frames B]
/// sql check   --db DIR --file q.sql [--json] [--frames B]
/// ```
/// `parse` needs no catalog and never opens a database (§M3.7). The text
/// may hold several statements separated by `;`.
/// - With `--explain`: the parse tree of each statement (explainAst), one
///   after the other.
/// - Without it: nothing is printed when the text parses.
///
/// `compile` and `check` (chunk 12) compile the whole file with compileSql
/// against the catalog of `--db` (`--catalog` is an alias; `--frames`,
/// default 64, only sizes the pool that reads the catalog; `--json` is
/// accepted and changes nothing). Nothing is executed.
/// - `compile`: the logical plan JSON of the query; several queries give a
///   JSON array in order; CREATE and INSERT print nothing.
/// - `check`: no output when the file is correct; otherwise the SQL error
///   on `out` (errors are its output) and exit 1.
///
/// Exit codes, as in the storage and exec tools: 0 ok; 1 an SQL error
/// (printed with formatError: `error: <category>: <message> at line L,
/// column C`; on `err`, except for `check`); 2 usage error (printed as
/// "usage error: ..." plus the usage lines); 3 internal error
/// (std::logic_error, a bug); any other failure, such as a file that cannot
/// be read, is 1 with "error: ..." (DECISIONS 2026-11-08 "sql CLI exit codes").

#include <ostream>
#include <string>
#include <vector>

namespace db::apps
{
    /// Runs the `sql` tool. `args` are the command-line arguments without
    /// the program name (`args[0]` is the command). Normal output goes to
    /// `out`, every error message to `err`. Never throws.
    int runSqlCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
} // namespace db::apps
