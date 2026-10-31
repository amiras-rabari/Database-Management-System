#pragma once

/// @file
/// The records inside `catalog.db` (chunk 11, "Version 2 (Simple)",
/// DECISIONS 2026-10-27). Private to storage: other modules only see
/// TableInfo / TableStats through the Catalog.
///
/// `catalog.db` is a row of ordinary slotted pages (chunk 03). Each live slot
/// holds one catalog record, and the record's first byte says what it is:
///
/// - kind 1, table record (one per table):
///   kind u8 = 1 | table name | data file name | first page u32 |
///   num pages u32 (data pages only) | num live tuples u64 | column count u16 |
///   per column: bare name, type tag u8 (0 INT, 1 VARCHAR), VARCHAR n u16 (0 for INT)
/// - kind 2, statistics chunk (one or more per analyzed table):
///   kind u8 = 2 | table name | chunk number u16 | chunk count u16 | payload bytes
///
/// A "name" is a u16 byte length followed by the bytes (the VARCHAR rule of
/// §M1.3). All integers are little-endian.
///
/// All of one table's statistics are first encoded as ONE byte string, the
/// "stats blob" (encodeStats). The blob is then cut by bytes into chunks so
/// that every record fits on one page (at most 4084 bytes, D5). Reading
/// joins the chunks back in order and decodes the blob once. The cutting
/// and joining knows nothing about statistics: it just moves bytes.
///
/// There is no header record: the number of catalog pages is the file's page
/// count, and the number of tables is the number of kind-1 records.
///
/// Errors: a record that is cut short, has bytes left over, or holds an
/// impossible value is a corrupt catalog file → std::runtime_error saying
/// what is wrong (D1). The Catalog adds the page and slot to the message.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "db/common/schema.h"
#include "db/storage/table_info.h"
#include "db/storage/table_stats.h"

namespace db::storage::catalog_format
{
    /// The first byte of every record.
    inline constexpr std::uint8_t kTableRecord = 1;
    inline constexpr std::uint8_t kStatsChunk = 2;

    /// Type tags of a column in a table record.
    inline constexpr std::uint8_t kIntTag = 0;
    inline constexpr std::uint8_t kVarcharTag = 1;

    /// The longest record that fits on an empty slotted page:
    /// 4096 − 8 (header) − 4 (its slot) = 4084 (D5).
    inline constexpr std::size_t kMaxCatalogRecord = 4084;

    /// The kind byte of a record. Empty record → std::runtime_error.
    std::uint8_t recordKind(std::span<const std::byte> record);

    // --- Table records --------------------------------------------------------

    /// The kind-1 record of `info`: the fields in the order above, columns
    /// stored bare (any qualifier is ignored). `info.stats` is not part of it.
    /// A name longer than 65535 bytes → std::logic_error (cannot be stored).
    std::vector<std::byte> encodeTableRecord(const TableInfo& info);

    /// The TableInfo a kind-1 record holds, with bare column names and
    /// `stats` empty. Corrupt or truncated record → std::runtime_error.
    TableInfo decodeTableRecord(std::span<const std::byte> record);

    // --- The stats blob ---------------------------------------------------------

    /// One table's statistics as one byte string:
    /// nTuples u64 | nPages u32 | width (bits of the double) u64 | column count u16 |
    /// per column: ndv u64, min, max, bucket count u16,
    ///   per bucket: lo, hi, count u64, ndv u64.
    /// min/max/lo/hi use the §M1.3 encoding of the column's type (INT 4 bytes;
    /// VARCHAR u16 length + bytes); the schema says which, as in a data record.
    /// Stats that do not match the schema (column count, a value of the
    /// wrong type, more than 65535 buckets) → std::logic_error.
    std::vector<std::byte> encodeStats(const TableStats& stats, const Schema& schema);

    /// The statistics back from a blob, using the table's schema for the
    /// value types. Corrupt or truncated blob → std::runtime_error.
    TableStats decodeStats(std::span<const std::byte> blob, const Schema& schema);

    // --- Cutting a blob into kind-2 records and joining it back --------------

    /// One decoded kind-2 record.
    struct StatsChunk
    {
        std::string tableName;
        std::uint16_t chunkNumber = 0;
        std::uint16_t chunkCount = 0;
        std::vector<std::byte> payload;

        bool operator==(const StatsChunk&) const = default;
    };

    /// How many payload bytes one kind-2 record of table `tableName` can carry:
    /// 4084 − (1 + 2 + name length + 2 + 2).
    std::size_t chunkPayloadCapacity(const std::string& tableName);

    /// Cuts `blob` into kind-2 records for `tableName`, chunk numbers
    /// 0 … count − 1, each record at most 4084 bytes. Every chunk but the last
    /// is completely full. An empty blob still gives one (empty) chunk, so
    /// "has statistics" is never lost. More than 65535 chunks, or a name so
    /// long that no payload fits → std::logic_error.
    std::vector<std::vector<std::byte>> cutIntoChunks(const std::string& tableName,
                                                      std::span<const std::byte> blob);

    /// Decodes one kind-2 record. Corrupt or truncated → std::runtime_error.
    StatsChunk decodeStatsChunk(std::span<const std::byte> record);

    /// Joins the chunks of one table back into its blob. `chunks` must be in
    /// the order they were read; they must be numbered 0, 1, …, count − 1 with
    /// the same count and table name in every chunk. A missing, extra or
    /// out-of-order chunk → std::runtime_error.
    std::vector<std::byte> joinChunks(const std::vector<StatsChunk>& chunks);
} // namespace db::storage::catalog_format
