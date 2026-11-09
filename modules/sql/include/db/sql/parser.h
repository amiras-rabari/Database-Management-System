#pragma once

/// @file
/// The SQL parser's public entry points: SQL text in, AST out (§M3.2).
/// No catalog is needed; names are not resolved here (chunks 07-10).

#include <db/sql/ast.h>

#include <string_view>
#include <vector>

namespace db::sql
{

    /// Parses a script: any number of statements separated by `;` (§M3.1).
    /// The final `;` is optional and empty statements (`;;`, or only
    /// comments) are skipped, so the shell can pass one statement without a
    /// `;`. An empty script gives an empty vector.
    ///
    /// Throws db::Error(Lexical) from the lexer, or db::Error(Syntax) at the
    /// first unexpected token ("expected X but found 'Y'"), or "nesting too
    /// deep" past kMaxNestingDepth. First error only (§M3.6).
    std::vector<Statement> parseSqlScript(std::string_view text);

    /// Parses exactly one query, optionally followed by `;` (for opt's
    /// tests and `sql parse --query`). Anything else, such as a CREATE or a
    /// second statement, is a syntax error. Same errors as parseSqlScript.
    QueryExpr parseSqlQuery(std::string_view text);

} // namespace db::sql
