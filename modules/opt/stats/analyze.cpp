#include "db/opt/analyze.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/external_sort.h"
#include "db/opt/histogram.h"
#include "db/opt/statistics.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/catalog.h"
#include "db/storage/heap_file.h"
#include "db/storage/heap_scan.h"
#include "db/storage/record_codec.h"
#include "db/storage/rid.h"
#include "db/storage/storage_engine.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::opt
{
    storage::TableStats analyzeTable(storage::StorageEngine& engine, const std::string& table, std::int32_t buckets)
    {
        // openHeapFile first: an unknown table throws storage's own
        // Error(Name) "Unknown table 'R'." before anything is read or stored.
        storage::HeapFile heap = engine.openHeapFile(table);
        const storage::TableInfo info = *engine.catalog().getTable(table);
        const Schema& schema = info.schema;

        // n comes from the catalog, which storage keeps exact on every insert
        // and load; the histogram needs it before the first value (chunk 02).
        // n_pages is the catalog's data page count (§M4.1: no directory pages).
        const std::uint64_t n = info.numTuples;
        storage::TableStats stats;
        stats.nTuples = n;
        stats.nPages = info.numPages;

        // How many columns one scan can sort at once. The scan holds 1 frame
        // and each sort needs at least 2 while it collects runs, so a scan
        // feeds (B - 1) / 2 sorts: 15 at B = 32 (one scan for most tables, as
        // §M4.1 says), 1 at B = 3 or 4 (one scan per column).
        const std::size_t frames = engine.pool().frameCount();
        const std::size_t columnsPerScan = std::max<std::size_t>(1, (frames - 1) / 2);

        std::uint64_t totalBytes = 0;
        std::uint64_t comparisons = 0; // the sorter counts them; ANALYZE does not report them

        for (std::size_t first = 0; first < schema.size(); first += columnsPerScan)
        {
            const std::size_t count = std::min(columnsPerScan, schema.size() - first);

            // Each sort gets one-column tuples: its run pages hold only that
            // column, so they are fewer than pages of whole rows. The schemas
            // and keys are built first, because the sorters keep references
            // to them; reserve() stops the vectors from moving them.
            std::vector<Schema> columnSchemas;
            std::vector<std::vector<SortKey>> columnKeys;
            columnSchemas.reserve(count);
            columnKeys.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const Column& column = schema.column(first + i);
                columnSchemas.push_back(Schema({column}));
                columnKeys.push_back({SortKey{ColumnRef{column.qualifier, column.name}, true}});
            }

            // While the scan runs, the B - 1 frames it does not hold are split
            // evenly between the sorts (B = 32, 2 columns: 15 each).
            // Afterwards each sort finishes alone, so it may merge with all B.
            const std::size_t runFrames = (frames - 1) / count;
            std::vector<std::unique_ptr<exec::ExternalSorter>> sorters;
            for (std::size_t i = 0; i < count; ++i)
            {
                sorters.push_back(std::make_unique<exec::ExternalSorter>(
                    engine, columnSchemas[i], columnKeys[i], runFrames, frames, frames, comparisons));
            }

            // One scan of the table feeds every sort of this group.
            {
                storage::HeapScan scan = heap.scan();
                storage::Rid rid;
                Tuple row;
                while (scan.next(rid, row))
                {
                    // Width (§M4.1 "average record length"): the record bytes
                    // of every row, added up during the first scan only.
                    if (first == 0)
                    {
                        totalBytes += storage::encodedLength(row, schema);
                    }
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        sorters[i]->add(Tuple{row[first + i]});
                    }
                }
                scan.close();
            }

            // The sorts finish one at a time, each with every frame. So every
            // sort except the first writes the values it still holds in
            // memory as a run now, and holds no frame while it waits.
            for (std::size_t i = 1; i < count; ++i)
            {
                sorters[i]->writeBufferedRun();
            }

            for (std::size_t i = 0; i < count; ++i)
            {
                exec::ExternalSorter& sorter = *sorters[i];
                sorter.finishInput();
                // The small lambda is the bridge between the two pieces: buildColumnStats asks "next value?"
                // , and the lambda pulls one tuple from the sorter and hands over its single value.
                stats.columns.push_back(buildColumnStats(schema.column(first + i).type, n, buckets,
                                                         [&sorter](Value& out)
                                                         {
                                                             Tuple sorted;
                                                             if (!sorter.next(sorted))
                                                             {
                                                                 return false;
                                                             }
                                                             out = sorted[0];
                                                             return true;
                                                         }));

                // Destroying the sorter deletes its runs and gives back its
                // frames before the next column's sort starts merging.
                sorters[i].reset();
            }
        }

        // An empty table has no records to average: use the schema's width
        // (the same rule as chunk 01's tableWidth).
        stats.width = n > 0 ? static_cast<double>(totalBytes) / static_cast<double>(n) : schemaWidth(schema);

        // Stores or replaces the table's statistics. They reach catalog.db at
        // the engine's next flushAll/close.
        engine.catalog().setStats(table, stats);
        return stats;
    }
} // namespace db::opt
