#pragma once

/// @file
/// Internal header of the SQL parser (not part of the public API).
/// Chunk 04 adds the public entry points in `db/sql/parser.h`.

#include <db/sql/ast.h>

#include <string_view>

namespace db::sql
{

    /// Parses `text` as one whole condition (the §M3.2 `condition` rule) and
    /// requires end of input after it. A test hook: WHERE, ON and HAVING use
    /// the same rules from inside the statement parser.
    /// Throws db::Error(Lexical) from the lexer, or db::Error(Syntax) at the
    /// first unexpected token, or "nesting too deep" past kMaxNestingDepth.
    SqlCondition parseConditionText(std::string_view text);

} // namespace db::sql
