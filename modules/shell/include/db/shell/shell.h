#pragma once

/// @file
/// The shell: one program with a prompt, where SQL goes in and rows come
/// out (integration SPEC). Each statement goes through every module: sql
/// compiles it, opt plans it, exec runs it, storage reads the pages.
///
/// Command line (the arguments without the program name):
/// ```
/// shell --db DIR [--frames B] [--file F] [--json]
/// ```
/// - `--db DIR` is required (`--catalog` is an alias, G5).
/// - `--frames B`: the memory budget, default 64, at least 3 (opt's cost
///   model divides by B - 2 and ANALYZE's external sort needs 3). The pool
///   uses LRU, the default of every tool.
/// - `--file F`: read the statements from F. Without it they come from
///   `in`, and a prompt is written to `err` before each line: `db> `, or
///   `...> ` while a statement is still open.
/// - `--json`: print one line of numbers per statement instead of rows.
///
/// Statements: CREATE TABLE, INSERT and SELECT (the sql module's subset),
/// plus `EXPLAIN q`, `EXPLAIN ANALYZE q`, `ANALYZE t`, `SET frames = n` and
/// `LOAD t FROM 'file'`. A failing statement prints its error on `err` and
/// the shell goes on with the next one.
///
/// Exit codes, as in the other four tools: 0 every statement worked; 1 at
/// least one failed (or the --file could not be read); 2 usage error;
/// 3 internal error (a bug).

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace db::shell
{
    /// Runs the shell on `in` (or the --file). Results go to `out`; errors
    /// and the prompt go to `err`. Never throws.
    int runShell(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err);
} // namespace db::shell
