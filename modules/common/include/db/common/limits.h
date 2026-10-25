#pragma once

/// @file
/// Limits shared by every parser and reader in the project.

namespace db
{

    /// Deepest tree the RA query parser, the condition parser and the plan
    /// JSON reader accept. Deeper input is an Error(Syntax) instead of a
    /// stack overflow.
    ///
    ///
    ///How depth is counted along any path from the root of the tree down to a leaf:
    // - Every plan operator that has inputs (select, project, join…) counts as 1.
    // - Every and / or / not inside a condition counts as 1.
    // - Table scans and comparisons like a = 1 are leaves and count as 0.
    // - While parsing, each ( also counts as 1, even though parentheses leave no node in the finished tree.
    // That way the recursion itself stays bounded.
    //
    /// The rule: along any root-to-leaf path of a parsed query, the number of
    /// plan levels plus the number of condition levels must not exceed this.
    /// A level is an operator node: a plan node with inputs, or an and/or/not
    /// condition node; scans and comparisons are leaves and add none. While
    /// parsing, every open parenthesis also counts as a level (it adds none
    /// to the finished tree), so the parsers' recursion stays bounded too.
    ///
    /// Trees deeper than this are not supported by the recursive algorithms
    /// (toString, outputSchema, ==, destructors, JSON): they may overflow the
    /// stack.
    ///
    ///
    /// constexpr: the value is known at compile time and can never change.
    /// It is like C's #define MAX 1000, but it has a type and a namespace, and the debugger can see it.
    /// - inline: this header is included by many .cpp files. inline (C++17) tells the linker that all those copies are one
    /// and the same variable, which is the correct way to define a constant in a header.
    inline constexpr int kMaxNestingDepth = 1000;

} // namespace db
