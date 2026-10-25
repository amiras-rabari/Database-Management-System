#pragma once

/// @file
/// The relational algebra query parser (the Bonus 1 query language plus
/// `distinct`), producing a plan tree.

#include <string_view>

#include "db/common/plan.h"

namespace db
{

    /// Parses a whole text as one relational algebra query.
    ///
    /// Grammar (lowest precedence first; every binary level is
    /// left-associative, so `A minus B minus C` is (A minus B) minus C and
    /// `A union B minus C` is (A union B) minus C):
    /// ```
    /// query      := expression End
    /// expression := intersect { ( "union" | "minus" ) intersect }
    /// intersect  := combine { "intersect" combine }
    /// combine    := unary { "times" unary | "join" "[" condition "]" unary }
    /// unary      := "select" "[" condition "]" "(" expression ")"
    ///             | "project" "[" column_ref { "," column_ref } "]" "(" expression ")"
    ///             | "rename" "[" identifier "]" "(" expression ")"
    ///             | "distinct" "(" expression ")"
    ///             | primary
    /// primary    := "(" expression ")" | identifier
    /// column_ref := name [ "." name ]
    /// ```
    /// `condition` is the grammar of parseCondition(). A relation or rename
    /// name is an identifier; a column name may also be an RA keyword. RA
    /// keywords are lowercase only (`SELECT` is a relation name).
    /// A relation name becomes Scan, `minus` Except and `times` Cross.
    ///
    /// Throws Error(Lexical) from tokenize(); Error(Syntax) at the offending
    /// token if the text is not exactly one query (including empty text,
    /// leftover tokens and an empty projection list).
    ///
    /// Depth follows the rule in limits.h, one budget for the plan and its
    /// conditions: along every path, each plan node with inputs (unary or
    /// binary operator) and each open `(` is one level, a scan is none, and
    /// the levels of a select or join condition count on top of the levels
    /// down to that node. So 1000 nested `(`, 1000 nested unary operators or
    /// a chain of 1000 binary operators (`R union R union ...`) parse; one
    /// more is Error(Syntax) "Query nested deeper than 1000 levels." at the
    /// `(`, keyword or operator that crosses the limit. When the limit is
    /// crossed inside a condition the message is the condition's ("Condition
    /// nested deeper than 1000 levels.").
    PlanNode parseRaQuery(std::string_view text);

} // namespace db
