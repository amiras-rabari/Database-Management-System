#include "db/storage/csv_loader.h"

#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/heap_file.h"
#include "db/storage/record_codec.h"
#include "db/storage/slotted_page.h"
#include "db/storage/table_info.h"
#include "loader/csv.h"

namespace db::storage
{
    namespace
    {
        // "line 7, column 2 (b): " — the start of every message about one
        // field. The name is left out when the field has no column (an extra
        // field past the end of the schema).
        std::string where(std::uint64_t line, int column, const Schema& schema)
        {
            std::string text = "line " + std::to_string(line) + ", column " + std::to_string(column);
            if (column >= 1 && static_cast<std::size_t>(column) <= schema.size())
                text += " (" + schema.column(static_cast<std::size_t>(column) - 1).name + ")";
            return text + ": ";
        }

        // db::Error stores line and column as int; a CSV with more than
        // INT_MAX lines would not fit, so clamp rather than wrap.
        int lineNumber(std::uint64_t line)
        {
            constexpr std::uint64_t kMax = 2147483647;
            return static_cast<int>(line < kMax ? line : kMax);
        }

        // Turns one line into a row of the schema, or throws the error that
        // names the line and the column.
        Tuple parseLine(const std::string& text, std::uint64_t line, const Schema& schema)
        {
            // 1. Split. The splitter knows the field number, not the line.
            std::vector<std::string> fields;
            try
            {
                fields = splitCsvLine(text);
            }
            catch (const Error& e)
            {
                throw Error(e.category(), where(line, e.column(), schema) + e.message(), lineNumber(line),
                            e.column());
            }

            // 2. Field count. The column is the first missing or extra field:
            //    2 fields for 3 columns → column 3; 5 fields → column 4.
            if (fields.size() != schema.size())
            {
                const std::size_t first = (fields.size() < schema.size() ? fields.size() : schema.size()) + 1;
                const int column = static_cast<int>(first);
                const std::string found = fields.empty() ? "a blank line" : std::to_string(fields.size()) + " fields";
                throw Error(ErrorCategory::Schema,
                            where(line, column, schema) + "expected " + std::to_string(schema.size())
                            + " fields, found " + found,
                            lineNumber(line), column);
            }

            // 3. Each field through common's parseValue, which keeps one rule
            //    for "valid INT" and "VARCHAR fits" in the whole project. Its
            //    category is kept; the message gains the line and column.
            Tuple row;
            row.reserve(fields.size());
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                const int column = static_cast<int>(i) + 1;
                try
                {
                    row.push_back(parseValue(fields[i], schema.column(i)));
                }
                catch (const Error& e)
                {
                    throw Error(e.category(), where(line, column, schema) + e.message(), lineNumber(line), column);
                }
            }
            return row;
        }
    } // namespace

    LoadResult loadCsv(StorageEngine& engine, const std::string& table, std::istream& in)
    {
        // 1. The table, and its tuple count before this load.
        const std::optional<TableInfo> info = engine.catalog().getTable(table);
        if (!info)
            throw Error(ErrorCategory::Name, "Unknown table '" + table + "'.");
        HeapFile heap = engine.openHeapFile(table);
        const Schema& schema = heap.schema();

        LoadResult result;
        // Whatever happens, the catalog must count the rows that were
        // inserted (rows before a bad line stay, STATUS 13b).
        auto saveCounts = [&] {
            if (result.rows > 0)
                engine.catalog().updateCounts(table, heap.numDataPages(), info->numTuples + result.rows);
        };

        try
        {
            // 2. One line at a time; `text` is reused, so memory stays at one
            //    line and one row however large the file is.
            std::string text;
            std::uint64_t line = 0;
            while (std::getline(in, text))
            {
                ++line; // lines are numbered from 1
                const Tuple row = parseLine(text, line, schema);

                // 3. A record that can never fit on a page (D5): name the
                //    table and the line, before anything is inserted.
                const std::size_t length = encodedLength(row, schema);
                if (length + kSlotSize > kPageSize - kPageHeaderSize)
                    throw Error(ErrorCategory::Schema,
                                "line " + std::to_string(line) + ": table '" + table + "': record too long ("
                                + std::to_string(length) + " bytes, at most "
                                + std::to_string(kPageSize - kPageHeaderSize - kSlotSize) + " fit on a page)",
                                lineNumber(line), 0);

                heap.insert(row);
                ++result.rows;
            }
            // getline also stops at end of file; bad() means a real read error.
            if (in.bad())
                throw std::runtime_error("CSV read failed after line " + std::to_string(line) + " of table '"
                    + table + "'");
        }
        catch (...)
        {
            saveCounts();
            throw;
        }

        saveCounts();
        return result;
    }

    LoadResult loadCsvFile(StorageEngine& engine, const std::string& table, const std::filesystem::path& path)
    {
        // Binary: the splitter strips the \r of "\r\n" itself, the same way
        // on every OS.
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw Error(ErrorCategory::Name, "Cannot open CSV file '" + path.string() + "'.");
        return loadCsv(engine, table, in);
    }
} // namespace db::storage
