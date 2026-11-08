#pragma once

/// @file
/// A position in the SQL text. The lexer stamps one on every token, and the
/// AST copies it into every node, so that errors found much later (name,
/// type, aggregate checks) can still say `at line L, column C` (§M3.6).

#include <cstdint>

namespace db::sql
{

    /// A 1-based line and column in the SQL text.
    /// The column counts bytes from the start of the line; a tab counts as
    /// one column. "\r\n" counts as one newline.
    struct SourcePos
    {
        int32_t line = 1;
        int32_t column = 1;

        bool operator==(const SourcePos&) const = default;
    };

} // namespace db::sql
