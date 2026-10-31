#include "storage_cli.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/csv_loader.h"
#include "db/storage/heap_file.h"
#include "db/storage/heap_scan.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/page_layout.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/rid.h"
#include "db/storage/schema_spec.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"
#include "db/storage/trace.h"

namespace db::apps
{
    namespace
    {
        using Json = nlohmann::ordered_json;
        using storage::IoStats;
        using storage::ReplacementPolicy;
        using storage::StorageEngine;

        /// Frames when `--frames` is omitted (STATUS 2026-10-27, chunk 14).
        constexpr std::size_t kDefaultFrames = 64;

        const char* const kUsage =
            "usage:\n"
            "  storage create-table --db DIR --name R --schema \"a:INT,b:VARCHAR(20),c:INT\" [--frames B]\n"
            "  storage load         --db DIR --table R --file r.csv [--frames B]\n"
            "  storage scan         --db DIR --table R [--frames B] [--limit N] [--policy lru|clock] [--json]\n"
            "  storage layout       --db DIR --table R --page P [--json]\n"
            "  storage trace        --db DIR --file trace.txt [--frames B] [--policy lru|clock] [--json]\n"
            "(--catalog is an alias of --db; --frames defaults to 64)\n";

        /// A bad command line: exit code 2, message + usage on `err`.
        class UsageError : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

        /// The parsed `--key value` pairs (keys without the dashes; `catalog`
        /// already renamed to `db`) and the `--json` flag.
        struct Options
        {
            std::map<std::string, std::string> values;
            bool json = false;

            bool has(const std::string& key) const { return values.count(key) > 0; }
            const std::string& get(const std::string& key) const { return values.at(key); }
        };

        /// One command: its name, the options it requires and allows (every
        /// command also takes `--db` (required) and `--frames`), whether it
        /// takes `--json`, and the function that runs it. Adding a command is
        /// one row in commands() plus one function.
        struct Command
        {
            std::string_view name;
            std::vector<std::string> required;
            std::vector<std::string> optional;
            bool takesJson;
            int (*run)(const Options&, std::ostream& out);
        };

        // --- option helpers --------------------------------------------------

        std::size_t framesOption(const Options& options)
        {
            if (!options.has("frames"))
                return kDefaultFrames;
            const std::optional<std::int32_t> n = parseInt32(options.get("frames"));
            if (!n || *n < 1)
                throw UsageError("--frames must be a whole number of at least 1, got '" + options.get("frames") + "'");
            return static_cast<std::size_t>(*n);
        }

        /// `--limit N` (N ≥ 0), or empty = no limit.
        std::optional<std::uint64_t> limitOption(const Options& options)
        {
            if (!options.has("limit"))
                return std::nullopt;
            const std::optional<std::int32_t> n = parseInt32(options.get("limit"));
            if (!n || *n < 0)
                throw UsageError("--limit must be a whole number of at least 0, got '" + options.get("limit") + "'");
            return static_cast<std::uint64_t>(*n);
        }

        /// `--policy lru|clock`; default LRU (DECISIONS 2026-10-25).
        ReplacementPolicy policyOption(const Options& options)
        {
            if (!options.has("policy"))
                return ReplacementPolicy::Lru;
            const std::optional<ReplacementPolicy> policy = storage::parsePolicy(options.get("policy"));
            if (!policy)
                throw UsageError("--policy must be lru or clock, got '" + options.get("policy") + "'");
            return *policy;
        }

        std::filesystem::path dbOption(const Options& options)
        {
            return std::filesystem::path(options.get("db"));
        }

        /// Scans and inserts pin one directory page and one data page together
        /// (§M1.5); with one frame the pool would fail on the second pin with
        /// an internal error, so say it plainly instead (exit code 1).
        void requireTwoFrames(std::size_t frames, const std::string& what)
        {
            if (frames < 2)
                throw std::runtime_error(what + " needs at least 2 frames (it pins one directory page and one data "
                                                "page at a time), got --frames "
                    + std::to_string(frames));
        }

        // --- commands --------------------------------------------------------

        // storage create-table --db DIR --name R --schema "a:INT,..."
        // Prints "created table R".
        int runCreateTable(const Options& options, std::ostream& out)
        {
            const std::size_t frames = framesOption(options);
            // Parse the schema first: a bad schema creates no folder or file.
            const Schema schema = storage::parseSchemaSpec(options.get("schema"));
            auto engine = StorageEngine::open(dbOption(options), frames, ReplacementPolicy::Lru);
            engine->catalog().createTable(options.get("name"), schema);
            engine->close();
            out << "created table " << options.get("name") << "\n";
            return 0;
        }

        // storage load --db DIR --table R --file r.csv
        // Prints "loaded N rows into R". On a bad line the rows before it stay
        // (the engine's destructor saves them; the error is still exit 1).
        int runLoad(const Options& options, std::ostream& out)
        {
            const std::size_t frames = framesOption(options);
            requireTwoFrames(frames, "a load");
            auto engine = StorageEngine::open(dbOption(options), frames, ReplacementPolicy::Lru);
            const storage::LoadResult result =
                storage::loadCsvFile(*engine, options.get("table"), std::filesystem::path(options.get("file")));
            engine->close();
            out << "loaded " << result.rows << " rows into " << options.get("table") << "\n";
            return 0;
        }

        // A column's entry in the scan JSON "schema": ["a","INT"] or
        // ["b","VARCHAR",20], with the bare name (getTable qualifies it).
        Json columnJson(const Column& column)
        {
            Json entry = Json::array({column.name, std::string(typeName(column.type))});
            if (column.type == Type::Varchar)
                entry.push_back(column.varcharLength);
            return entry;
        }

        // A row of the scan JSON "rows": INT as numbers, VARCHAR as strings.
        Json rowJson(const Tuple& row)
        {
            Json values = Json::array();
            for (const Value& value : row)
            {
                if (value.type() == Type::Int)
                    values.push_back(value.asInt());
                else
                    values.push_back(value.asVarchar());
            }
            return values;
        }

        // The scan JSON "stats", keys in the spec's order.
        Json statsJson(const IoStats& stats)
        {
            Json json = Json::object();
            json["hits"] = stats.hits;
            json["misses"] = stats.misses;
            json["evictions"] = stats.evictions;
            json["reads"] = stats.reads;
            json["writes"] = stats.writes;
            return json;
        }

        // A row in the readable form: values separated by " | ", VARCHAR
        // values as they are (no quotes).
        std::string rowText(const Tuple& row)
        {
            std::string text;
            for (std::size_t i = 0; i < row.size(); ++i)
            {
                if (i > 0)
                    text += " | ";
                text += row[i].type() == Type::Int ? std::to_string(row[i].asInt()) : row[i].asVarchar();
            }
            return text;
        }

        // storage scan --db DIR --table R [--frames B] [--limit N] [--policy P] [--json]
        //
        // Counters (D4): the snapshot is taken after open and after the table
        // lookup, so the catalog pages read by open are not in them. The scan
        // is lazy and stops right after row N, so `--limit 0` reads nothing.
        //
        // Rows are printed as they are read, never collected, so the command
        // holds one row at a time (the B-page rule). The JSON is written in
        // pieces that together are exactly ordered_json's compact dump() of
        // {"table", "schema", "rows", "stats"} on one line.
        int runScan(const Options& options, std::ostream& out)
        {
            const std::size_t frames = framesOption(options);
            const std::optional<std::uint64_t> limit = limitOption(options);
            const ReplacementPolicy policy = policyOption(options);
            requireTwoFrames(frames, "a scan");

            const std::string& table = options.get("table");
            auto engine = StorageEngine::open(dbOption(options), frames, policy);
            const std::optional<storage::TableInfo> info = engine->catalog().getTable(table);
            if (!info)
                throw Error(ErrorCategory::Name, "Unknown table '" + table + "'.");
            const storage::HeapFile heap = engine->openHeapFile(table);
            const IoStats before = engine->pool().stats();

            if (options.json)
            {
                Json schema = Json::array();
                for (const Column& column : info->schema.columns())
                    schema.push_back(columnJson(column));
                out << "{\"table\":" << Json(table).dump() << ",\"schema\":" << schema.dump() << ",\"rows\":[";
            }
            else
            {
                out << "table " << table << " (";
                for (std::size_t i = 0; i < info->schema.size(); ++i)
                {
                    const Column& column = info->schema.column(i);
                    out << (i > 0 ? ", " : "") << column.name << " " << typeName(column.type);
                    if (column.type == Type::Varchar)
                        out << "(" << column.varcharLength << ")";
                }
                out << ")\n";
            }

            std::uint64_t count = 0;
            {
                storage::HeapScan scan = heap.scan();
                storage::Rid rid;
                Tuple row;
                // Check the limit before asking for the next row, so no page
                // is fetched after row N.
                while ((!limit || count < *limit) && scan.next(rid, row))
                {
                    if (options.json)
                        out << (count > 0 ? "," : "") << rowJson(row).dump();
                    else
                        out << rowText(row) << "\n";
                    ++count;
                }
                scan.close();
            }
            const IoStats stats = engine->pool().stats() - before;

            if (options.json)
                out << "],\"stats\":" << statsJson(stats).dump() << "}\n";
            else
                out << count << (count == 1 ? " row" : " rows") << "\n"
                    << "stats: hits=" << stats.hits << " misses=" << stats.misses << " evictions=" << stats.evictions
                    << " reads=" << stats.reads << " writes=" << stats.writes << "\n";

            engine->close();
            return 0;
        }

        /// `--page P` (P ≥ 0).
        storage::PageId pageOption(const Options& options)
        {
            const std::optional<std::int32_t> n = parseInt32(options.get("page"));
            if (!n || *n < 0)
                throw UsageError("--page must be a whole number of at least 0, got '" + options.get("page") + "'");
            return static_cast<storage::PageId>(*n);
        }

        // A tuple as "(42, 'bob', 7)" for the layout text: VARCHAR values in
        // single quotes, so an empty string or one with ", " stays readable.
        std::string tupleText(const Tuple& row)
        {
            std::string text = "(";
            for (std::size_t i = 0; i < row.size(); ++i)
            {
                text += i > 0 ? ", " : "";
                text += row[i].type() == Type::Int ? row[i].toText() : "'" + row[i].asVarchar() + "'";
            }
            return text + ")";
        }

        // One header line of the layout text: offset, size, field, value.
        void headerLine(std::ostream& out, std::size_t offset, int size, const std::string& field,
                        const std::string& value)
        {
            out << std::setw(6) << offset << std::setw(6) << size << "  " << std::left << std::setw(14) << field
                << std::right << value << "\n";
        }

        void printDataPageText(const storage::PageLayout& page, std::ostream& out)
        {
            out << "offset  size  field         value\n";
            headerLine(out, storage::kDataPageIdOffset, 4, "page_id", std::to_string(page.pageId));
            headerLine(out, storage::kDataSlotCountOffset, 2, "slot_count", std::to_string(page.slotCount));
            headerLine(out, storage::kDataFreeEndOffset, 2, "free_end", std::to_string(page.freeEnd));
            out << "free space = free_end - (8 + 4 x slot_count) = " << page.freeSpace << " bytes\n";
            out << "slot  at  offset  length  state  record\n";
            for (const storage::SlotLayout& slot : page.slots)
            {
                out << std::setw(4) << slot.slot << std::setw(4) << slot.entryOffset << std::setw(8) << slot.offset
                    << std::setw(8) << slot.length << "  " << (slot.live ? "live " : "dead ");
                if (slot.live)
                    out << "  bytes " << slot.offset << ".." << (slot.offset + slot.length - 1) << "  "
                        << tupleText(slot.values);
                out << "\n";
            }
        }

        void printDirectoryPageText(const storage::PageLayout& page, std::ostream& out)
        {
            out << "offset  size  field         value\n";
            headerLine(out, storage::kDirPageIdOffset, 4, "page_id", std::to_string(page.pageId));
            headerLine(out, storage::kDirNextPageOffset, 4, "next_dir_page",
                page.nextDirPage == storage::kInvalidPageId ? "none (0xFFFFFFFF)" : std::to_string(page.nextDirPage));
            headerLine(out, storage::kDirEntryCountOffset, 2, "entry_count", std::to_string(page.entryCount));
            out << "entry  at  data_page  free_bytes\n";
            for (const storage::DirEntryLayout& entry : page.entries)
                out << std::setw(5) << entry.index << std::setw(4) << entry.entryOffset << std::setw(11)
                    << entry.dataPageId << std::setw(12) << entry.freeBytes << "\n";
        }

        // storage layout --db DIR --table R --page P [--json]
        //
        // One page, slot by slot (or entry by entry for a directory page), with
        // the byte offset of every header field and slot entry, so it can be
        // compared with the diagram in DESIGN_M1.md. JSON field names:
        // STATUS 2026-10-27 (15a).
        int runLayout(const Options& options, std::ostream& out)
        {
            const std::size_t frames = framesOption(options);
            const storage::PageId pageId = pageOption(options);
            auto engine = StorageEngine::open(dbOption(options), frames, ReplacementPolicy::Lru);
            const storage::PageLayout page = storage::describePage(*engine, options.get("table"), pageId);
            engine->close();

            const bool isData = page.kind == storage::PageKind::Data;
            if (options.json)
            {
                Json json = Json::object();
                json["table"] = page.table;
                json["page_id"] = page.pageId;
                json["kind"] = isData ? "data" : "directory";
                if (isData)
                {
                    json["slot_count"] = page.slotCount;
                    json["free_end"] = page.freeEnd;
                    json["free_space"] = page.freeSpace;
                    Json slots = Json::array();
                    for (const storage::SlotLayout& slot : page.slots)
                        slots.push_back(Json{{"slot", slot.slot}, {"offset", slot.offset}, {"length", slot.length},
                            {"live", slot.live}});
                    json["slots"] = slots;
                }
                else
                {
                    json["next_dir_page"] = page.nextDirPage;
                    json["entry_count"] = page.entryCount;
                    Json entries = Json::array();
                    for (const storage::DirEntryLayout& entry : page.entries)
                        entries.push_back(Json{{"index", entry.index}, {"data_page_id", entry.dataPageId},
                            {"free_bytes", entry.freeBytes}});
                    json["entries"] = entries;
                }
                out << json.dump() << "\n";
                return 0;
            }

            out << "table " << page.table << ", page " << pageId << " of " << page.filePages << " ("
                << (isData ? "data" : "directory") << " page)\n";
            if (isData)
                printDataPageText(page, out);
            else
                printDirectoryPageText(page, out);
            return 0;
        }

        // Reads a trace file: one page id per line. Blank lines (only spaces,
        // tabs or a '\r') are skipped and a missing final newline is fine
        // (STATUS 2026-10-27, 15d). Anything else that is not a whole number
        // ≥ 0 is an error naming the line. The ids are numbers, not pages, so
        // keeping them in a vector does not count against B.
        std::vector<storage::PageId> readTrace(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw Error(ErrorCategory::Name, "Cannot open trace file '" + path.string() + "'.");
            std::vector<storage::PageId> ids;
            std::string line;
            int lineNumber = 0;
            while (std::getline(in, line))
            {
                ++lineNumber;
                const std::size_t first = line.find_first_not_of(" \t\r");
                if (first == std::string::npos)
                    continue; // blank line
                const std::size_t last = line.find_last_not_of(" \t\r");
                const std::string text = line.substr(first, last - first + 1);
                const std::optional<std::int32_t> id = parseInt32(text);
                if (!id || *id < 0)
                    throw Error(ErrorCategory::Syntax,
                        "Trace file '" + path.string() + "': '" + text + "' is not a page id (a whole number >= 0).",
                        lineNumber, 0);
                ids.push_back(static_cast<storage::PageId>(*id));
            }
            return ids;
        }

        // storage trace --db DIR --file trace.txt [--frames B] [--policy P] [--json]
        //
        // Replays the page ids through a fresh pool: each page is fetched and
        // unpinned clean at once (§M1.9), so writes is always 0 and
        // reads == misses. Field order as §M1.9.
        int runTrace(const Options& options, std::ostream& out)
        {
            const std::size_t frames = framesOption(options);
            const ReplacementPolicy policy = policyOption(options);
            const std::vector<storage::PageId> ids = readTrace(std::filesystem::path(options.get("file")));
            const IoStats stats = storage::runTrace(dbOption(options), ids, frames, policy);

            if (options.json)
            {
                Json json = Json::object();
                json["policy"] = std::string(storage::policyName(policy));
                json["frames"] = frames;
                json["hits"] = stats.hits;
                json["misses"] = stats.misses;
                json["evictions"] = stats.evictions;
                json["reads"] = stats.reads;
                json["writes"] = stats.writes;
                out << json.dump() << "\n";
            }
            else
                out << "trace " << options.get("file") << ": " << ids.size() << " requests, policy "
                    << storage::policyName(policy) << ", " << frames << " frames\n"
                    << "hits=" << stats.hits << " misses=" << stats.misses << " evictions=" << stats.evictions
                    << " reads=" << stats.reads << " writes=" << stats.writes << "\n";
            return 0;
        }

        // --- dispatch --------------------------------------------------------

        const std::vector<Command>& commands()
        {
            static const std::vector<Command> table = {
                {"create-table", {"name", "schema"}, {}, false, &runCreateTable},
                {"load", {"table", "file"}, {}, false, &runLoad},
                {"scan", {"table"}, {"limit", "policy"}, true, &runScan},
                {"layout", {"table", "page"}, {}, true, &runLayout},
                {"trace", {"file"}, {"policy"}, true, &runTrace},
            };
            return table;
        }

        bool listed(const std::vector<std::string>& keys, const std::string& key)
        {
            return std::find(keys.begin(), keys.end(), key) != keys.end();
        }

        // Reads `args[1..]` as options of `command`. Every problem is a
        // UsageError.
        Options parseOptions(const Command& command, const std::vector<std::string>& args)
        {
            Options options;
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                const std::string& arg = args[i];
                if (arg.size() < 3 || arg.compare(0, 2, "--") != 0)
                    throw UsageError("expected an option starting with --, got '" + arg + "'");
                std::string key = arg.substr(2);

                if (key == "json")
                {
                    if (!command.takesJson)
                        throw UsageError("unknown option --json for " + std::string(command.name));
                    options.json = true;
                    continue;
                }
                if (key == "catalog")
                    key = "db"; // alias (G5)
                if (key != "db" && key != "frames" && !listed(command.required, key) && !listed(command.optional, key))
                    throw UsageError("unknown option " + arg + " for " + std::string(command.name));
                if (i + 1 >= args.size() || args[i + 1].compare(0, 2, "--") == 0)
                    throw UsageError("missing value for " + arg);
                if (options.has(key))
                    throw UsageError("option --" + key + " given twice (--catalog is the same as --db)");
                options.values[key] = args[i + 1];
                ++i;
            }

            if (!options.has("db"))
                throw UsageError("missing required option --db");
            for (const std::string& key : command.required)
                if (!options.has(key))
                    throw UsageError("missing required option --" + key);
            return options;
        }

        int dispatch(const std::vector<std::string>& args, std::ostream& out)
        {
            if (args.empty())
                throw UsageError("missing command");
            for (const Command& command : commands())
                if (args[0] == command.name)
                    return command.run(parseOptions(command, args), out);
            throw UsageError("unknown command '" + args[0] + "'");
        }
    } // namespace

    int runStorageCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err)
    {
        // Every exception ends here; the engine (if any) was already
        // destroyed on the way out, which saves what it can.
        try
        {
            return dispatch(args, out);
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
} // namespace db::apps
