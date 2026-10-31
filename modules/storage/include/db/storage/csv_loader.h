#pragma once

/// @file
/// The CSV bulk loader of §M1.7 (chunk 13). A library function, so the
/// `storage load` command and the shell's `LOAD t FROM f` run the same code.
///
/// Dialect: no header line, one record per line, fields separated by commas,
/// a field containing a comma wrapped in double quotes (`""` inside quotes is
/// one `"`), spaces kept as data, one trailing `\r` ignored, no field spanning
/// lines; a blank line is a row with the wrong number of fields. Each field
/// becomes a value through common's `parseValue` (the project's one rule for
/// "valid INT" and "VARCHAR fits").
///
/// Errors stop the load. Every error about a line is a db::Error with
/// line() = the 1-based line number and, when one field is to blame,
/// column() = its 1-based field number; the message starts
/// "line L, column C (name): ...". Rows read before the bad line stay in the
/// table, and the catalog's page and tuple counts are updated to match them
/// before the error is rethrown.
///
/// Memory: the input is read one line at a time; only the current line and
/// its row are held.

#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>

#include "db/storage/storage_engine.h"

namespace db::storage
{
    /// What a load did.
    struct LoadResult
    {
        /// Rows inserted by this load.
        std::uint64_t rows = 0;
    };

    /// Appends every line of `in` to table `table` of `engine`, then updates
    /// the catalog counts (data pages, old tuple count + rows inserted).
    ///
    /// Errors:
    /// - unknown table → db::Error(Name) "Unknown table 'X'." (nothing read);
    /// - unterminated quote / text after a closing quote → Error(Syntax);
    /// - wrong number of fields (a blank line included) → Error(Schema),
    ///   column = position of the first missing or extra field;
    /// - a field `parseValue` rejects → its category (Type for a bad INT,
    ///   Schema for a VARCHAR longer than n), message naming the column;
    /// - a record longer than 4084 bytes → Error(Schema) naming the table and
    ///   the line (checked before inserting it);
    /// - a stream read failure → std::runtime_error.
    LoadResult loadCsv(StorageEngine& engine, const std::string& table, std::istream& in);

    /// Opens `path` (binary mode, so `\r` handling is the same on every OS) and
    /// calls loadCsv. Cannot open → db::Error(Name) naming the path.
    LoadResult loadCsvFile(StorageEngine& engine, const std::string& table, const std::filesystem::path& path);
} // namespace db::storage
