#pragma once

/// @file
/// How the SQL front end reaches the catalog, without linking storage.

#include <db/common/schema.h>

#include <functional>
#include <optional>
#include <string>

namespace db::sql
{

    /// Finds a table's schema by name, exactly as written (identifiers are
    /// case-sensitive: `emp` does not find `Emp`).
    /// - Empty means "no such table"; the caller reports the error.
    /// - Otherwise the columns are qualified with the table name, as
    ///   storage's `Catalog::getTable` returns them (`Emp.name`).
    /// - It must not read data pages: only the catalog (DEPENDENCIES S8).
    /// The same shape as the lookup of common's `outputSchema`, so one
    /// lookup serves both. The CLI and the shell wrap `Catalog::getTable` in
    /// a lambda; tests use a map (`tests/test_catalog.h`).
    using TableLookup = std::function<std::optional<Schema>(const std::string&)>;

} // namespace db::sql
