#pragma once

/// @file
/// The `exec` command-line tool (SPEC §M2.5, exec chunk 14) as a function,
/// so tests can run it with string streams. `exec_main.cpp` only calls it
/// with std::cout / std::cerr. All printing and all output formats are here,
/// in apps/, never in the library.
///
/// Commands:
/// ```
/// exec run   --db DIR --plan plan.json [--frames B] [--json]
/// exec trace --db DIR --plan plan.json [--frames B] [--json]
/// ```
/// `--catalog` is an alias of `--db`; `--frames` defaults to 64, like the
/// storage tool. The buffer pool uses LRU (DECISIONS 2026-10-25).
///
/// Output:
/// - `run --json`: §M2.5's object `{"schema": [...], "rows": [...],
///   "totals": {"tuples_out", "comparisons", "page_reads", "page_writes"}}`.
///   Rows are written one by one as they come out of the plan (never all
///   kept in memory). An AVG (REAL) value is a JSON number with exactly
///   four decimals, e.g. 3.5000 (§M2.3).
/// - `run` (text, chunk 14 Q1 (a)): a line of column names, one line per
///   row, values separated by " | ", then a line of totals.
/// - `trace`: the plan runs to the end, its rows are not printed (§M2.5
///   shows counters only). One line per node, in plan order, indented two
///   spaces per depth: label, own tuples_out and comparisons, then the
///   page reads and writes of the node and everything below it (D1 (b)).
/// - `trace --json`: {"nodes": [{depth, label, tuples_out, comparisons,
///   page_reads, page_writes, subtree: {the four}, details: {...}}],
///   "totals": {...}, "max_frames_held": n}. `details` holds a sort's
///   passes and a hash join's partitions (§M2.6 measurements 2 and 4);
///   max_frames_held is the most frames held at once (measurement 5).
///
/// Exit codes, as in the storage tool: 0 ok; 1 user error (db::Error,
/// printed with formatError, or a file that cannot be read); 2 usage error
/// (printed as "usage error: ..." plus the usage lines); 3 internal error
/// (std::logic_error: a bug).

#include <ostream>
#include <string>
#include <vector>

namespace db::apps
{
    /// Runs the `exec` tool. `args` are the command-line arguments without
    /// the program name (`args[0]` is the command). Normal output goes to
    /// `out`, every error message to `err`. Never throws.
    int runExecCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
} // namespace db::apps
