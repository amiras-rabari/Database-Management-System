#pragma once

// Private to db_common: the condition grammar over a token vector, shared by
// parseCondition() and the RA query parser (for `select[...]` and `join[...]`).
//
// Contract for a caller such as the RA parser:
// - It owns `tokens` (from tokenize(), so the last token is End) and a cursor
//   `position` into them.
// - parseConditionAt() parses one condition starting at tokens[position] and
//   leaves `position` on the first token after it (for the RA parser: the
//   `]`). It never moves past End and never looks at tokens before `position`.
// - It stops at the first token that cannot continue the condition; checking
//   what that token is (`]`, End, ...) is the caller's job.
// - Every syntax error is Error(Syntax) carrying the line and column of the
//   offending token, built by throwSyntaxError(). Lexical errors cannot occur
//   here: they were raised by tokenize().
// - Depth (the rule in limits.h) is one budget shared with the caller:
//   `startDepth` is the number of levels already used above the condition
//   (for the RA parser: the levels down to and including the select or join
//   node that holds it). The condition's own levels are counted on top, and
//   going past kMaxNestingDepth is "Condition nested deeper than 1000
//   levels.". The returned height is the number of levels the condition
//   adds, so the caller can count them in its own node's height.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "db/common/condition.h"
#include "db/common/token.h"

namespace db::detail
{

    // A parsed condition and the number of levels it adds (its and/or/not
    // nodes along the deepest path; 0 for a single comparison).
    struct ParsedCondition
    {
        Condition condition;
        int height = 0;
    };

    // Parses one condition at tokens[position] and advances `position` past
    // it. Preconditions: tokens.back() is End, position < tokens.size(), and
    // 0 <= startDepth <= kMaxNestingDepth. Throws Error(Syntax) at the
    // offending token.
    ParsedCondition parseConditionAt(const std::vector<Token>& tokens, std::size_t& position, int startDepth = 0);

    // True for the tokens that can be a column or relation name in a
    // condition: an identifier or an RA keyword, never and/or/not.
    bool isNameToken(TokenKind kind);

    // How a token is named in an error message: `'b'`, or `end of input`.
    std::string describeToken(const Token& token);

    // Throws Error(Syntax) "<expected>, found <token>." at the token's position.
    [[noreturn]] void throwSyntaxError(const Token& token, std::string_view expected);

} // namespace db::detail
