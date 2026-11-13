#pragma once

/// @file
/// The `opt` command-line tool (SPEC §M4.6, opt chunk 14) as a function, so
/// tests can run it with string streams. `opt_main.cpp` only calls it with
/// std::cout / std::cerr. All option parsing and printing live here, in
/// apps/; the optimizer library returns values and text.
///
/// Commands (§M4.6; `--catalog` is an alias of `--db`, G5):
/// ```
/// opt analyze  --db DIR --table R [--buckets 20] [--frames B] [--json]
/// opt plan     --db DIR --logical q.json [--frames B] [--json] [switches]
/// opt explain  --db DIR --logical q.json [--frames B] [--json] [--show-rewrite] [switches]
/// opt dp-table --db DIR --logical q.json [--frames B] [--json] [--disable-rule N]...
/// ```
/// `--frames` defaults to 64 (as in the storage and exec tools) and must be
/// at least 3 (ANALYZE uses exec's external sort, which needs 3 frames);
/// `--buckets` defaults to 20 (§M4.1) and must be at least 1. The buffer
/// pool uses LRU (DECISIONS 2026-10-25).
///
/// Switches for the §M4.7 measurements (ours, not in the spec):
/// - `--disable-rule N` (N = 1..7, may be repeated): turn rewrite rule N
///   off (§M4.7 #3);
/// - `--unoptimized`: the unoptimized plan of §M4.7 #2 (written join
///   order, nested loop joins, no pushdown). Not with `--disable-rule`
///   (unoptimized mode sets the rules itself) and not for `dp-table`
///   (there is no DP);
/// - `--show-rewrite` (explain only, text only): the logical tree before
///   and after the rewrite rules (§M4.3), then the physical plan.
///
/// Output:
/// - `analyze`: `analyzed R: 1000 rows, 10 pages, width 40.96`; with
///   `--json` the same as `{"table", "rows", "pages", "width"}`.
/// - `plan`: the physical plan JSON (common's toJsonText) and nothing else,
///   so it can be saved to a file and run by `exec` unedited (§M4.6).
///   `--json` changes nothing: JSON is its only output.
/// - `explain`: one line per physical node with est_rows and est_cost
///   (explainText). With `--json`: an array with one
///   `{"est_rows", "est_cost"}` object per node, in pre-order, the same
///   order as `exec trace --json`'s nodes, which hold the depth and label.
/// - `dp-table`: the DP table of each join block (dpTableText), separated by
///   a blank line. `--json` changes nothing.
/// The same files, flags and B always give the same output.
///
/// Exit codes, as in the storage, exec and sql tools: 0 ok; 1 user error
/// (db::Error, printed with formatError, or a file that cannot be read);
/// 2 usage error ("usage error: ..." plus the usage lines); 3 internal
/// error (std::logic_error: a bug).

#include <ostream>
#include <string>
#include <vector>

namespace db::apps
{
    /// Runs the `opt` tool. `args` are the command-line arguments without
    /// the program name (`args[0]` is the command). Normal output goes to
    /// `out`, every error message to `err`. Never throws.
    int runOptCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
} // namespace db::apps
