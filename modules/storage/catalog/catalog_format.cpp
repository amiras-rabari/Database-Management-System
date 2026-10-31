#include "catalog/catalog_format.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "db/common/error.h"
#include "db/storage/bytes.h"

namespace db::storage::catalog_format
{
    namespace
    {
        constexpr std::size_t kU16Max = std::numeric_limits<std::uint16_t>::max();

        // --- Writing: append fields to the end of a growing byte vector ------
        // Each helper makes room at the end, then writes the field there with
        // the chunk 01 helpers (so the byte order rule lives in one place).

        void appendU8(std::vector<std::byte>& out, std::uint8_t value)
        {
            out.push_back(static_cast<std::byte>(value));
        }

        void appendU16(std::vector<std::byte>& out, std::uint16_t value)
        {
            const std::size_t at = out.size();
            out.resize(at + 2);
            writeU16(out, at, value);
        }

        void appendU32(std::vector<std::byte>& out, std::uint32_t value)
        {
            const std::size_t at = out.size();
            out.resize(at + 4);
            writeU32(out, at, value);
        }

        void appendU64(std::vector<std::byte>& out, std::uint64_t value)
        {
            const std::size_t at = out.size();
            out.resize(at + 8);
            writeU64(out, at, value);
        }

        // A u16 length, then the bytes (the VARCHAR rule of §M1.3).
        // `what` names the text in the error message.
        void appendText(std::vector<std::byte>& out, const std::string& text, const std::string& what)
        {
            if (text.size() > kU16Max)
                throw std::logic_error("Catalog: " + what + " is " + std::to_string(text.size())
                    + " bytes; at most 65535 can be stored");
            appendU16(out, static_cast<std::uint16_t>(text.size()));
            for (char c : text)
                out.push_back(static_cast<std::byte>(c));
        }

        // A min/max/lo/hi value in the §M1.3 encoding of `column`'s type.
        // The value must have the column's type: the reader decides how to
        // decode it from the schema alone, so a mismatch would be unreadable.
        void appendValue(std::vector<std::byte>& out, const Value& value, const Column& column,
                         const std::string& what)
        {
            if (value.type() != column.type)
                throw std::logic_error("Catalog: statistics " + what + " for column '" + column.name
                    + "' is " + std::string(typeName(value.type())) + " but the column is "
                    + std::string(typeName(column.type)));
            if (column.type == Type::Int)
            {
                const std::size_t at = out.size();
                out.resize(at + 4);
                writeI32(out, at, value.asInt());
            }
            else
                appendText(out, value.asVarchar(), "statistics " + what + " for column '" + column.name + "'");
        }

        // --- Reading: walk a read-only view with a running offset --------------
        // Every read first checks that enough bytes are left. A short record is
        // a corrupt file (std::runtime_error), not a bug, so this check comes
        // before the bytes.h helpers (whose own check throws std::logic_error).
        class Reader
        {
        public:
            Reader(std::span<const std::byte> bytes, std::string what)
                : bytes_(bytes), what_(std::move(what))
            {
            }

            std::uint8_t u8(const char* field)
            {
                need(1, field);
                return std::to_integer<std::uint8_t>(bytes_[offset_++]);
            }

            std::uint16_t u16(const char* field)
            {
                need(2, field);
                const std::uint16_t value = readU16(bytes_, offset_);
                offset_ += 2;
                return value;
            }

            std::uint32_t u32(const char* field)
            {
                need(4, field);
                const std::uint32_t value = readU32(bytes_, offset_);
                offset_ += 4;
                return value;
            }

            std::int32_t i32(const char* field)
            {
                need(4, field);
                const std::int32_t value = readI32(bytes_, offset_);
                offset_ += 4;
                return value;
            }

            std::uint64_t u64(const char* field)
            {
                need(8, field);
                const std::uint64_t value = readU64(bytes_, offset_);
                offset_ += 8;
                return value;
            }

            // A u16 length, then that many bytes. The length is checked
            // against the bytes left before anything is copied.
            std::string text(const char* field)
            {
                const std::uint16_t length = u16(field);
                need(length, field);
                std::string result;
                result.reserve(length);
                for (std::size_t i = 0; i < length; ++i)
                    result.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes_[offset_ + i])));
                offset_ += length;
                return result;
            }

            // A value of `column`'s type, encoded as in appendValue.
            Value value(const Column& column, const char* field)
            {
                if (column.type == Type::Int)
                    return Value::ofInt(i32(field));
                return Value::ofVarchar(text(field));
            }

            // Everything from here to the end (a chunk's payload).
            std::vector<std::byte> rest()
            {
                std::vector<std::byte> result(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), bytes_.end());
                offset_ = bytes_.size();
                return result;
            }

            // Every byte must have been used: bytes left over mean the record
            // is not what we think it is.
            void requireEnd() const
            {
                if (offset_ != bytes_.size())
                    throw std::runtime_error("Corrupt catalog: " + what_ + " has "
                        + std::to_string(bytes_.size() - offset_) + " bytes left over after its last field");
            }

            // Throws a "corrupt" error if a later check finds an impossible value.
            [[noreturn]] void corrupt(const std::string& problem) const
            {
                throw std::runtime_error("Corrupt catalog: " + what_ + ": " + problem);
            }

        private:
            // Throws unless `size` more bytes are left. Written as a
            // subtraction (offset_ never passes bytes_.size()), so it cannot wrap.
            void need(std::size_t size, const char* field) const
            {
                if (bytes_.size() - offset_ < size)
                    throw std::runtime_error("Corrupt catalog: " + what_ + " is cut short (" + field
                        + " needs " + std::to_string(size) + " bytes at offset " + std::to_string(offset_)
                        + ", only " + std::to_string(bytes_.size() - offset_) + " left)");
            }

            std::span<const std::byte> bytes_;
            std::string what_;
            std::size_t offset_ = 0;
        };

        // Bytes of a kind-2 record before its payload.
        std::size_t chunkHeaderSize(const std::string& tableName)
        {
            return 1 + 2 + tableName.size() + 2 + 2;
        }
    } // namespace

    std::uint8_t recordKind(std::span<const std::byte> record)
    {
        if (record.empty())
            throw std::runtime_error("Corrupt catalog: empty record (no kind byte)");
        return std::to_integer<std::uint8_t>(record[0]);
    }

    // --- Table records ----------------------------------------------------------

    std::vector<std::byte> encodeTableRecord(const TableInfo& info)
    {
        std::vector<std::byte> out;
        appendU8(out, kTableRecord);
        appendText(out, info.name, "table name");
        appendText(out, info.dataFile, "data file name");
        appendU32(out, info.firstPage);
        appendU32(out, info.numPages);
        appendU64(out, info.numTuples);
        if (info.schema.size() > kU16Max)
            throw std::logic_error("Catalog: table '" + info.name + "' has more than 65535 columns");
        appendU16(out, static_cast<std::uint16_t>(info.schema.size()));
        for (const Column& column : info.schema.columns())
        {
            // Bare name only: the qualifier is added back by getTable.
            appendText(out, column.name, "column name");
            appendU8(out, column.type == Type::Int ? kIntTag : kVarcharTag);
            appendU16(out, static_cast<std::uint16_t>(column.varcharLength));
        }
        return out;
    }

    TableInfo decodeTableRecord(std::span<const std::byte> record)
    {
        Reader in(record, "table record");
        if (in.u8("kind") != kTableRecord)
            in.corrupt("kind byte is not 1");

        TableInfo info;
        info.name = in.text("table name");
        info.dataFile = in.text("data file name");
        info.firstPage = in.u32("first page");
        info.numPages = in.u32("num pages");
        info.numTuples = in.u64("num tuples");

        const std::uint16_t columnCount = in.u16("column count");
        if (columnCount == 0)
            in.corrupt("table '" + info.name + "' has no columns");
        std::vector<Column> columns;
        for (std::uint16_t i = 0; i < columnCount; ++i)
        {
            Column column;
            column.name = in.text("column name");
            const std::uint8_t tag = in.u8("column type tag");
            if (tag == kIntTag)
                column.type = Type::Int;
            else if (tag == kVarcharTag)
                column.type = Type::Varchar;
            else
                in.corrupt("column '" + column.name + "' has unknown type tag " + std::to_string(tag));
            column.varcharLength = in.u16("VARCHAR length");
            columns.push_back(std::move(column));
        }
        in.requireEnd();

        // The Schema constructor checks the rest (VARCHAR n in 1..255, INT
        // length 0, no duplicate names). Its db::Error means bad data in the
        // file here, so it becomes a "corrupt catalog" error.
        try
        {
            info.schema = Schema(std::move(columns));
        }
        catch (const Error& error)
        {
            in.corrupt("table '" + info.name + "' has an impossible schema (" + error.message() + ")");
        }
        return info;
    }

    // --- The stats blob -------------------------------------------------------------

    std::vector<std::byte> encodeStats(const TableStats& stats, const Schema& schema)
    {
        if (stats.columns.size() != schema.size())
            throw std::logic_error("Catalog: statistics have " + std::to_string(stats.columns.size())
                + " columns but the table has " + std::to_string(schema.size()));

        std::vector<std::byte> out;
        appendU64(out, stats.nTuples);
        appendU32(out, stats.nPages);
        // The double's 8 raw bytes, as an integer: exact round trip.
        appendU64(out, std::bit_cast<std::uint64_t>(stats.width));
        appendU16(out, static_cast<std::uint16_t>(stats.columns.size()));
        for (std::size_t i = 0; i < stats.columns.size(); ++i)
        {
            const ColumnStats& col = stats.columns[i];
            const Column& column = schema.column(i);
            appendU64(out, col.ndv);
            appendValue(out, col.min, column, "min");
            appendValue(out, col.max, column, "max");
            if (col.histogram.size() > kU16Max)
                throw std::logic_error("Catalog: column '" + column.name + "' has more than 65535 buckets");
            appendU16(out, static_cast<std::uint16_t>(col.histogram.size()));
            for (const HistogramBucket& bucket : col.histogram)
            {
                appendValue(out, bucket.lo, column, "bucket lo");
                appendValue(out, bucket.hi, column, "bucket hi");
                appendU64(out, bucket.count);
                appendU64(out, bucket.ndv);
            }
        }
        return out;
    }

    TableStats decodeStats(std::span<const std::byte> blob, const Schema& schema)
    {
        Reader in(blob, "statistics");
        TableStats stats;
        stats.nTuples = in.u64("nTuples");
        stats.nPages = in.u32("nPages");
        stats.width = std::bit_cast<double>(in.u64("width"));
        const std::uint16_t columnCount = in.u16("column count");
        if (columnCount != schema.size())
            in.corrupt("they have " + std::to_string(columnCount) + " columns but the table has "
                + std::to_string(schema.size()));
        for (std::uint16_t i = 0; i < columnCount; ++i)
        {
            const Column& column = schema.column(i);
            ColumnStats col;
            col.ndv = in.u64("ndv");
            col.min = in.value(column, "min");
            col.max = in.value(column, "max");
            const std::uint16_t buckets = in.u16("bucket count");
            for (std::uint16_t b = 0; b < buckets; ++b)
            {
                HistogramBucket bucket;
                bucket.lo = in.value(column, "bucket lo");
                bucket.hi = in.value(column, "bucket hi");
                bucket.count = in.u64("bucket count of rows");
                bucket.ndv = in.u64("bucket ndv");
                col.histogram.push_back(std::move(bucket));
            }
            stats.columns.push_back(std::move(col));
        }
        in.requireEnd();
        return stats;
    }

    // --- Chunks -------------------------------------------------------------------

    std::size_t chunkPayloadCapacity(const std::string& tableName)
    {
        const std::size_t header = chunkHeaderSize(tableName);
        return header >= kMaxCatalogRecord ? 0 : kMaxCatalogRecord - header;
    }

    std::vector<std::vector<std::byte>> cutIntoChunks(const std::string& tableName,
                                                      std::span<const std::byte> blob)
    {
        const std::size_t capacity = chunkPayloadCapacity(tableName);
        if (capacity == 0)
            throw std::logic_error("Catalog: table name '" + tableName + "' is too long for a statistics record");

        // ceil(size / capacity) chunks, but at least one (an empty blob still
        // says "this table has statistics").
        const std::size_t count = std::max<std::size_t>(1, (blob.size() + capacity - 1) / capacity);
        if (count > kU16Max)
            throw std::logic_error("Catalog: statistics of table '" + tableName + "' need "
                + std::to_string(count) + " records; at most 65535 are allowed");

        std::vector<std::vector<std::byte>> records;
        for (std::size_t number = 0; number < count; ++number)
        {
            // Chunk `number` carries blob bytes [start, start + length).
            const std::size_t start = number * capacity;
            const std::size_t length = std::min(capacity, blob.size() - start);

            std::vector<std::byte> record;
            appendU8(record, kStatsChunk);
            appendText(record, tableName, "table name");
            appendU16(record, static_cast<std::uint16_t>(number));
            appendU16(record, static_cast<std::uint16_t>(count));
            record.insert(record.end(), blob.begin() + static_cast<std::ptrdiff_t>(start),
                          blob.begin() + static_cast<std::ptrdiff_t>(start + length));
            records.push_back(std::move(record));
        }
        return records;
    }

    StatsChunk decodeStatsChunk(std::span<const std::byte> record)
    {
        Reader in(record, "statistics record");
        if (in.u8("kind") != kStatsChunk)
            in.corrupt("kind byte is not 2");
        StatsChunk chunk;
        chunk.tableName = in.text("table name");
        chunk.chunkNumber = in.u16("chunk number");
        chunk.chunkCount = in.u16("chunk count");
        if (chunk.chunkCount == 0 || chunk.chunkNumber >= chunk.chunkCount)
            in.corrupt("chunk " + std::to_string(chunk.chunkNumber) + " of "
                + std::to_string(chunk.chunkCount) + " is impossible");
        chunk.payload = in.rest(); // payload length = record length - header
        return chunk;
    }

    std::vector<std::byte> joinChunks(const std::vector<StatsChunk>& chunks)
    {
        if (chunks.empty())
            throw std::runtime_error("Corrupt catalog: no statistics chunks to join");
        const StatsChunk& first = chunks.front();
        const std::string what = "Corrupt catalog: statistics of table '" + first.tableName + "'";
        if (chunks.size() != first.chunkCount)
            throw std::runtime_error(what + " have " + std::to_string(chunks.size()) + " chunks, expected "
                + std::to_string(first.chunkCount));

        std::vector<std::byte> blob;
        for (std::size_t i = 0; i < chunks.size(); ++i)
        {
            // Chunk i must be number i, so a missing chunk or two chunks in
            // the wrong order are both caught here.
            const StatsChunk& chunk = chunks[i];
            if (chunk.tableName != first.tableName || chunk.chunkCount != first.chunkCount
                || chunk.chunkNumber != i)
                throw std::runtime_error(what + ": chunk " + std::to_string(i) + " is out of order or missing");
            blob.insert(blob.end(), chunk.payload.begin(), chunk.payload.end());
        }
        return blob;
    }
} // namespace db::storage::catalog_format
