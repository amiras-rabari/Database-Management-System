#include "db/opt/statistics.h"

#include <cstddef>
#include <vector>

#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/page.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    double tableWidth(const storage::TableInfo& info)
    {
        if (info.stats)
        {
            return info.stats->width;
        }

        if (info.numTuples > 0)
        {
            // Bytes in the table's data pages, spread over its rows. This
            // counts page headers and free space too, so it is a little more
            // than the records alone; in return pages = ceil(rows x width /
            // 4096) gives back the real page count for a whole table.
            double bytes = static_cast<double>(info.numPages) * static_cast<double>(storage::kPageSize);
            return bytes / static_cast<double>(info.numTuples);
        }

        // Empty and never analyzed: the widest record the schema allows.
        // An INT takes 4 bytes; a VARCHAR(n) a 2-byte length plus up to n bytes.
        double width = 0.0;
        for (const Column& column : info.schema.columns())
        {
            if (column.type == Type::Varchar)
            {
                width += 2.0 + column.varcharLength;
            }
            else
            {
                width += 4.0;
            }
        }
        return width;
    }

    std::vector<double> columnWidths(const storage::TableInfo& info)
    {
        const std::vector<Column>& columns = info.schema.columns();

        // Count the VARCHAR columns; every other table column is an INT
        // (a table column is never REAL).
        std::size_t varcharCount = 0;
        for (const Column& column : columns)
        {
            if (column.type == Type::Varchar)
            {
                ++varcharCount;
            }
        }

        // The INT columns take 4 bytes each; the VARCHAR columns share what
        // is left of the table width. Emp, width 30: 30 - 4 x 4 = 14 for name.
        double varcharWidth = 0.0;
        if (varcharCount > 0)
        {
            double intBytes = 4.0 * static_cast<double>(columns.size() - varcharCount);
            varcharWidth = (tableWidth(info) - intBytes) / static_cast<double>(varcharCount);
        }

        std::vector<double> widths;
        widths.reserve(columns.size());
        for (const Column& column : columns)
        {
            widths.push_back(column.type == Type::Varchar ? varcharWidth : 4.0);
        }
        return widths;
    }

    const storage::ColumnStats* columnStats(const storage::TableInfo& info, const ColumnRef& ref)
    {
        // Resolve first, so an unknown column is an error even on a table
        // that was never analyzed.
        std::size_t position = info.schema.resolve(ref);

        if (!info.stats)
        {
            return nullptr;
        }

        // Storage keeps one ColumnStats per column, in schema order, so the
        // column's schema position is its index here.
        const storage::ColumnStats& column = info.stats->columns[position];
        if (column.ndv == 0)
        {
            return nullptr;
        }
        return &column;
    }
} // namespace db::opt
