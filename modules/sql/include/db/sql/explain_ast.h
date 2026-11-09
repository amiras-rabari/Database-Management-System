#pragma once

/// @file
/// Prints a parse tree as indented text, for `sql parse --explain` (§M3.7:
/// "indented tree, no catalog needed"). It shows what the parser built, so
/// a reader can see the precedence and grouping the grammar chose.

#include <db/sql/ast.h>

#include <string>

namespace db::sql
{

    /// One line per node, two spaces of indent per level, each line ending
    /// in '\n'. The root line names the statement: `select_block`, a set
    /// operator (`union`, `union_all`, `intersect`, `except`, so example
    /// 5.7's two queries differ in exactly that line), `create_table` or
    /// `insert`. Example for `SELECT name FROM Emp WHERE age > 30`:
    /// ```
    /// select_block
    ///   item: name
    ///   from
    ///     table: Emp
    ///   where: age > 30
    /// ```
    /// A condition is printed on one line, and every AND, OR and NOT below
    /// the top gets parentheses: `a = 1 OR (b = 2 AND (NOT c = 3))`.
    /// Keywords are upper case; a string literal is quoted with each ' doubled.
    /// Never throws for a tree the parser built.
    std::string explainAst(const Statement& statement);

} // namespace db::sql
