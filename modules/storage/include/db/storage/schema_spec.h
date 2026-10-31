#pragma once

/// @file
/// The schema text of `storage create-table --schema` (§M1.9, chunk 14):
/// `a:INT,b:VARCHAR(20),c:INT`. In the library so doctest can test it like
/// any other parser; split by hand (no <regex>).

#include <string_view>

#include "db/common/schema.h"

namespace db::storage
{
    /// Turns `text` into a Schema of bare columns (empty qualifier), in order.
    ///
    /// Grammar: items separated by `,`; each item is `name:INT` or
    /// `name:VARCHAR(n)`. Type names are case-insensitive (`int`, `Varchar(5)`).
    /// Spaces and tabs around a name or a type are ignored. A name is letters,
    /// digits and `_` (the same characters as a table name). n is read by
    /// parseInt32 and must be 1..255.
    ///
    /// Errors, all db::Error(Schema) naming the item (e.g. "Schema item 2
    /// 'b:VARCHAR(x)': ..."): empty text, empty item, missing `:`, empty or
    /// invalid name, unknown type, bad or out-of-range n; a duplicate name →
    /// the Schema constructor's "Duplicate column 'a'.".
    Schema parseSchemaSpec(std::string_view text);
} // namespace db::storage
