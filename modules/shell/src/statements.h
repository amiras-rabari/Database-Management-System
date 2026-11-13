#pragma once

/// @file
/// Running one statement (private to the shell). shell.cpp splits the input
/// into statements; runStatement carries out one of them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "db/common/error.h"
#include "db/exec/counters.h"
#include "db/exec/executable_plan.h"
#include "db/opt/optimizer.h"
#include "db/sql/lexer.h"
#include "db/storage/storage_engine.h"

namespace db::shell
{
    /// What one shell run keeps from one statement to the next.
    struct Session
    {
        storage::StorageEngine& engine;
        /// B: --frames at the start, changed by `SET frames = n`.
        std::int32_t frames = 0;
        /// --json: print numbers only, so no rows and no message lines.
        bool json = false;
        /// Results.
        std::ostream& out;
        /// Errors and the prompt.
        std::ostream& err;
        /// Statements so far (1, 2, ...), for the --json lines.
        int statementNumber = 0;
        /// Statements that failed; any failure makes the exit code 1.
        int failures = 0;
    };

    /// The numbers of one SELECT or EXPLAIN ANALYZE, for the --json line.
    struct QueryRun
    {
        /// opt's physical plan and one estimate per node, in pre-order.
        opt::OptimizedPlan optimized;
        /// exec's counters, one row per node in the same pre-order, so
        /// trace[i] and optimized.estimates[i] are the same node.
        std::vector<exec::TraceRow> trace;
        /// The whole plan: reads and writes summed over every node,
        /// tuplesOut = the root's = the number of result rows.
        exec::ExecCounters totals;
        /// From the start of the statement to its last row.
        double wallMs = 0.0;
    };

    /// Carries out the statement made of tokens[first, end) of `piece`
    /// (tokens[end] is its `;` or the end of the piece). Prints its output
    /// on session.out (nothing with --json). Returns the query's numbers
    /// for a SELECT or EXPLAIN ANALYZE, else nothing.
    ///
    /// Throws db::Error with a position inside `piece` (shell.cpp moves it
    /// to the whole script), std::runtime_error from storage for a broken
    /// file, std::logic_error for a bug.
    std::optional<QueryRun> runStatement(Session& session, std::string_view piece,
                                         const std::vector<sql::SqlToken>& tokens, std::size_t first,
                                         std::size_t end);

    /// `error` with its position moved by a text that starts at
    /// (line, column) of a bigger text: line L of the small text is line
    /// L + line - 1 of the big one, and only on its first line do columns
    /// move too. An error with no position (line 0) is returned unchanged.
    Error movedError(const Error& error, int line, int column);
} // namespace db::shell
