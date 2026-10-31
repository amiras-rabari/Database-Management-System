#pragma once

/// @file
/// The `storage` command-line tool (§M1.9, storage chunk 14) as a function,
/// so tests can run it with string streams. `storage_main.cpp` only calls it
/// with std::cout / std::cerr. All printing and all JSON code are here, in
/// apps/, never in the library.
///
/// Commands (options are `--key value` pairs, plus the bare flag `--json`):
/// ```
/// storage create-table --db DIR --name R --schema "a:INT,b:VARCHAR(20),c:INT"
/// storage load         --db DIR --table R --file r.csv
/// storage scan         --db DIR --table R [--frames B] [--limit N] [--policy lru|clock] [--json]
/// ```
/// `--catalog` is an alias of `--db`. `--frames B` (B ≥ 1, default 64) is
/// accepted on every command; a scan needs B ≥ 2.
///
/// Exit codes: 0 ok; 1 user error (db::Error, printed with formatError, or
/// another runtime error printed as "error: ..."); 2 usage error (unknown
/// command or option, missing value, missing required option, bad number or
/// policy; printed as "usage error: ..." plus a usage line); 3 internal
/// error (std::logic_error: a bug, printed as "internal error: ...").

#include <ostream>
#include <string>
#include <vector>

namespace db::apps
{
    /// Runs the `storage` tool. `args` are the command-line arguments
    /// without the program name (`args[0]` is the command). Normal output
    /// goes to `out`, every error message to `err`. Never throws.
    int runStorageCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
} // namespace db::apps
