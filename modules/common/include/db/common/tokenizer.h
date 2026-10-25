#pragma once

/// @file
/// The tokenizer for relational algebra queries and conditions.

#include <string_view>
#include <vector>

#include "db/common/token.h"

namespace db
{

    /// Splits `text` into tokens; the last token is always End.
    ///
    /// - Whitespace is any of space, `\t`, `\n`, `\v`, `\f`, `\r`; `\n` starts
    ///   a new line. `//` starts a comment that runs to the end of the line.
    /// - An identifier is an ASCII letter followed by letters, digits or `_`.
    ///   A word that is exactly an RA keyword (`select project rename join
    ///   union intersect minus times distinct`, lowercase) becomes that
    ///   keyword; `and`, `or`, `not` in any case become And, Or, Not.
    /// - An integer is a digit sequence; a `+` or `-` directly followed by a
    ///   digit belongs to it (`Age>-30` gives `>` then `-30`). Its value is
    ///   read by parseInt32.
    /// - A string is `'...'` on one line; `''` inside it stands for one quote.
    ///
    /// Throws Error(Lexical) at the offending position for: an unterminated
    /// string, a sign not followed by a digit, `!` not followed by `=`, a word
    /// that starts with a digit (`123abc`), an integer outside the INT range,
    /// and any other character (including `{`, `}` and a single `/`).
    std::vector<Token> tokenize(std::string_view text);

} // namespace db
