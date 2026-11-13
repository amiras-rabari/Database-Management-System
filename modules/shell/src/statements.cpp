#include "statements.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/counters.h"
#include "db/exec/exec_context.h"
#include "db/exec/executable_plan.h"
#include "db/opt/analyze.h"
#include "db/opt/defaults.h"
#include "db/opt/estimate.h"
#include "db/opt/explain.h"
#include "db/opt/optimizer.h"
#include "db/sql/compile.h"
#include "db/sql/lexer.h"
#include "db/sql/source_pos.h"
#include "db/sql/table_lookup.h"
#include "db/storage/catalog.h"
#include "db/storage/csv_loader.h"
#include "db/storage/heap_file.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::shell
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // True when `token` is the word `word` in any case. EXPLAIN, ANALYZE,
        // SET, LOAD and frames are not SQL keywords, so the lexer gives them
        // as identifiers spelled as written.
        bool isWord(const sql::SqlToken& token, std::string_view word)
        {
            if (token.kind != sql::SqlTokenKind::Identifier || token.text.size() != word.size())
                return false;
            for (std::size_t i = 0; i < word.size(); ++i)
            {
                char c = token.text[i];
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
                if (c != word[i])
                    return false;
            }
            return true;
        }

        // The byte offset of `pos` in `text`. The lexer counts a line per
        // '\n' and a column per byte, so the offset is the start of that
        // line plus column - 1.
        std::size_t offsetOf(std::string_view text, sql::SourcePos pos)
        {
            std::size_t offset = 0;
            for (int32_t line = 1; line < pos.line; ++line)
                offset = text.find('\n', offset) + 1;
            return offset + static_cast<std::size_t>(pos.column - 1);
        }

        // Compiles tokens[first, end) of `piece` with sql's compileSql. The
        // text handed to sql starts at tokens[first], so sql's positions are
        // moved back into the piece. Only the catalog is consulted, and it
        // already holds every table an earlier statement created.
        sql::CompiledStatement compile(Session& session, std::string_view piece,
                                       const std::vector<sql::SqlToken>& tokens, std::size_t first, std::size_t end)
        {
            const std::size_t begin = offsetOf(piece, tokens[first].pos);
            const std::string_view text = piece.substr(begin, offsetOf(piece, tokens[end].pos) - begin);
            const storage::Catalog& catalog = session.engine.catalog();
            const sql::TableLookup lookup = [&catalog](const std::string& name) -> std::optional<Schema>
            {
                const std::optional<storage::TableInfo> table = catalog.getTable(name);
                if (!table)
                    return std::nullopt;
                return table->schema;
            };
            try
            {
                // The text holds no ';', so it is exactly one statement.
                return sql::compileSql(text, lookup).front();
            }
            catch (const Error& error)
            {
                throw movedError(error, tokens[first].pos.line, tokens[first].pos.column);
            }
        }

        std::string rowCountText(std::uint64_t rows)
        {
            return std::to_string(rows) + (rows == 1 ? " row\n" : " rows\n");
        }

        // Plans and runs one query: sql's logical plan -> opt -> exec ->
        // storage. With `printRows`, writes the column names, then one line
        // per row as it arrives (values joined by " | ", like `storage scan`
        // and `exec run`). Rows are never stored: tuples kept in C++ would
        // count against B (§0.2).
        QueryRun runQuery(Session& session, const PlanNode& logical, Clock::time_point start, bool printRows)
        {
            storage::StorageEngine& engine = session.engine;
            // opt reads each table's catalog entry (rows, pages, statistics);
            // this costs no page I/O.
            const opt::StatsProvider stats = [&engine](const std::string& name) {
                return engine.catalog().getTable(name);
            };
            opt::OptimizerOptions options;
            options.frames = session.frames;
            QueryRun run;
            run.optimized = opt::optimize(logical, stats, options);

            // Cold pool: no page of an earlier statement stays cached and the
            // counters start at zero, so the same query always gives the same
            // numbers (§0.2), comparable with opt's estimates.
            engine.resetPool(static_cast<std::size_t>(session.frames));

            // The context is declared before the plan, which refers to it:
            // locals are destroyed in reverse order, so the plan goes first.
            exec::ExecContext context(engine);
            exec::ExecutablePlan plan = exec::buildExecutablePlan(run.optimized.physical, context);
            plan.open();
            if (printRows)
            {
                const std::vector<Column>& columns = plan.schema().columns();
                for (std::size_t i = 0; i < columns.size(); ++i)
                    session.out << (i == 0 ? "" : " | ") << columns[i].qualifiedName();
                session.out << "\n";
            }
            Tuple row; // one tuple, reused for every row
            while (plan.next(row))
            {
                if (!printRows)
                    continue;
                for (std::size_t i = 0; i < row.size(); ++i)
                    session.out << (i == 0 ? "" : " | ") << row[i].toText();
                session.out << "\n";
            }
            plan.close();
            run.wallMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            run.totals = plan.totals();
            run.trace = plan.trace();
            return run;
        }

        // EXPLAIN ANALYZE's text: opt's explain line i, then the real
        // numbers of exec's trace row i (the same node, by pre-order index;
        // not by label, since a self-join has two `seqscan` lines alike):
        // the node's own rows, the reads and writes of its whole subtree
        // (the convention of `exec trace`, DECISIONS 2026-11-01). Then one
        // line with the query's numbers.
        std::string explainAnalyzeText(const QueryRun& run)
        {
            std::istringstream explain(
                opt::explainText(run.optimized.physical, run.optimized.estimates, true));
            std::ostringstream text;
            std::string line;
            for (std::size_t i = 0; std::getline(explain, line); ++i)
            {
                const exec::TraceRow& node = run.trace[i];
                text << line << " actual_rows=" << node.counters.tuplesOut << " reads=" << node.subtree.pageReads
                     << " writes=" << node.subtree.pageWrites << "\n";
            }
            text << "rows=" << run.totals.tuplesOut << " page_io=" << run.totals.pageReads + run.totals.pageWrites
                 << " wall_ms=" << std::fixed << std::setprecision(3) << run.wallMs
                 << " est_root_rows=" << run.optimized.estimates[0].rows
                 << " actual_root_rows=" << run.totals.tuplesOut << "\n";
            return text.str();
        }

        // ANALYZE t: opt's exact statistics, stored in the catalog. The line
        // is the one `opt analyze` prints (width to 6 significant digits).
        std::string analyze(Session& session, const std::string& table)
        {
            const storage::TableStats stats = opt::analyzeTable(session.engine, table, opt::kDefaultBuckets);
            std::ostringstream text;
            text << "analyzed " << table << ": " << stats.nTuples << " rows, " << stats.nPages << " pages, width "
                 << stats.width << "\n";
            return text.str();
        }

        // INSERT: sql checked the row; storage stores it. The catalog's
        // counts are kept by whoever inserts (storage INTERFACE.md).
        std::string insert(Session& session, const sql::InsertPlan& plan)
        {
            storage::HeapFile heap = session.engine.openHeapFile(plan.table);
            const std::uint64_t oldTuples = session.engine.catalog().getTable(plan.table)->numTuples;
            heap.insert(plan.tuple);
            session.engine.catalog().updateCounts(plan.table, heap.numDataPages(), oldTuples + 1);
            return "inserted 1 row into " + plan.table + "\n";
        }
    } // namespace

    Error movedError(const Error& error, int line, int column)
    {
        if (error.line() == 0)
            return error;
        const int movedColumn = error.line() == 1 ? error.column() + column - 1 : error.column();
        return Error(error.category(), error.message(), error.line() + line - 1, movedColumn);
    }

    std::optional<QueryRun> runStatement(Session& session, std::string_view piece,
                                         const std::vector<sql::SqlToken>& tokens, std::size_t first,
                                         std::size_t end)
    {
        // Wall time covers the whole statement: compile to last row.
        const Clock::time_point start = Clock::now();
        const sql::SqlToken& word = tokens[first];
        const std::size_t count = end - first;
        // The text of a one-line result; written at the end unless --json.
        std::string text;
        std::optional<QueryRun> run;

        if (isWord(word, "ANALYZE"))
        {
            if (count != 2 || tokens[first + 1].kind != sql::SqlTokenKind::Identifier)
                throw Error(ErrorCategory::Syntax, "expected ANALYZE table", word.pos.line, word.pos.column);
            text = analyze(session, tokens[first + 1].text);
        }
        else if (isWord(word, "SET"))
        {
            // SET frames = n, n at least 3 (the same limit as --frames).
            if (count != 4 || !isWord(tokens[first + 1], "FRAMES") ||
                tokens[first + 2].kind != sql::SqlTokenKind::Equal ||
                tokens[first + 3].kind != sql::SqlTokenKind::Integer || tokens[first + 3].intValue < 3)
                throw Error(ErrorCategory::Syntax, "expected SET frames = n, with n a whole number of at least 3",
                            word.pos.line, word.pos.column);
            // Safe between statements: nothing holds a page now.
            session.frames = tokens[first + 3].intValue;
            session.engine.resetPool(static_cast<std::size_t>(session.frames));
            text = "frames = " + std::to_string(session.frames) + "\n";
        }
        else if (isWord(word, "LOAD"))
        {
            // LOAD t FROM 'path': storage opens the file and checks every
            // line. A relative path is relative to where the shell runs.
            if (count != 4 || tokens[first + 1].kind != sql::SqlTokenKind::Identifier ||
                tokens[first + 2].kind != sql::SqlTokenKind::From ||
                tokens[first + 3].kind != sql::SqlTokenKind::String)
                throw Error(ErrorCategory::Syntax, "expected LOAD table FROM 'file'", word.pos.line, word.pos.column);
            const std::string& table = tokens[first + 1].text;
            const storage::LoadResult loaded =
                storage::loadCsvFile(session.engine, table, std::filesystem::path(tokens[first + 3].text));
            text = "loaded " + std::to_string(loaded.rows) + " rows into " + table + "\n";
        }
        else if (isWord(word, "EXPLAIN"))
        {
            const bool analyzeToo = count > 1 && isWord(tokens[first + 1], "ANALYZE");
            const std::size_t query = first + (analyzeToo ? 2 : 1);
            if (query == end)
                throw Error(ErrorCategory::Syntax, "EXPLAIN needs a query", word.pos.line, word.pos.column);
            const sql::CompiledStatement compiled = compile(session, piece, tokens, query, end);
            const PlanNode* logical = std::get_if<PlanNode>(&compiled);
            if (logical == nullptr)
                throw Error(ErrorCategory::Syntax, "EXPLAIN needs a query", word.pos.line, word.pos.column);
            if (analyzeToo)
            {
                // Runs exactly like a SELECT, counting the rows instead of
                // printing them.
                run = runQuery(session, *logical, start, false);
                text = explainAnalyzeText(*run);
            }
            else
            {
                // EXPLAIN shows opt's physical plan; nothing runs.
                const opt::StatsProvider stats = [&session](const std::string& name) {
                    return session.engine.catalog().getTable(name);
                };
                opt::OptimizerOptions options;
                options.frames = session.frames;
                const opt::OptimizedPlan optimized = opt::optimize(*logical, stats, options);
                text = opt::explainText(optimized.physical, optimized.estimates, true);
            }
        }
        else
        {
            // Plain SQL: CREATE TABLE, INSERT or a query.
            const sql::CompiledStatement compiled = compile(session, piece, tokens, first, end);
            if (const auto* create = std::get_if<sql::CreateTablePlan>(&compiled))
            {
                session.engine.catalog().createTable(create->table, create->schema);
                text = "created table " + create->table + "\n";
            }
            else if (const auto* insertPlan = std::get_if<sql::InsertPlan>(&compiled))
                text = insert(session, *insertPlan);
            else
            {
                run = runQuery(session, std::get<PlanNode>(compiled), start, !session.json);
                text = rowCountText(run->totals.tuplesOut);
            }
        }

        if (!session.json)
            session.out << text;
        return run;
    }
} // namespace db::shell
