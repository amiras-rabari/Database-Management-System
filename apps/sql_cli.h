#pragma once

/// @file
/// The `sql` command-line tool (SPEC §M3.7, sql chunk 05) as a function, so
/// tests can run it with string streams. `sql_main.cpp` only calls it with
/// std::cout / std::cerr. All printing is here, in apps/, never in the
/// library.
///
/// Commands (chunk 12 adds `compile` and `check`):
/// ```
/// sql parse --query "SQL" [--explain]
/// ```
/// `parse` needs no catalog and never opens a database (§M3.7). The text
/// may hold several statements separated by `;`.
/// - With `--explain`: the parse tree of each statement (explainAst), one
///   after the other.
/// - Without it: nothing is printed when the text parses.
///
/// Exit codes, as in the storage and exec tools: 0 ok; 1 an SQL error
/// (lexical or syntax, printed with formatError: `error: <category>:
/// <message> at line L, column C`); 2 usage error (printed as
/// "usage error: ..." plus the usage lines); 3 internal error (a bug).

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
