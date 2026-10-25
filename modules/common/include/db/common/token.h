#pragma once

/// @file
/// The tokens of the query and condition language, produced by tokenize()
/// (tokenizer.h).

#include <cstdint>
#include <string>

namespace db
{

    /// What a token is. RA keywords match only their exact lowercase spelling;
    /// the condition keywords `and`, `or`, `not` match in any case.
    enum class TokenKind
    {
        // RA query keywords (lowercase only).
        Select,
        Project,
        Rename,
        Join,
        Union,
        Intersect,
        Minus,
        Times,
        Distinct,

        // Condition keywords (any case).
        And,
        Or,
        Not,

        // Names and literals.
        Identifier,
        Integer,
        String,

        // Comparison operators; `<>` and `!=` are both NotEqual.
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,

        // Punctuation.
        LeftParen,
        RightParen,
        LeftBracket,
        RightBracket,
        Comma,
        Dot,

        /// End of the input; always the last token, never anywhere else.
        End
    };

    /// One token with its position in the input.
    struct Token
    {
        /// What the token is.
        TokenKind kind = TokenKind::End;
        /// The exact characters of the token in the input (a string keeps its
        /// quotes and doubled quotes). Empty for End.
        std::string text;
        /// The value of an Integer token; 0 for every other kind.
        std::int32_t intValue = 0;
        /// The decoded content of a String token (`'O''Brien'` gives
        /// `O'Brien`); empty for every other kind.
        std::string stringValue;
        /// 1-based line of the token's first character (for End: of the
        /// position just after the input).
        int line = 1;
        /// 1-based column (in bytes) of the token's first character.
        int column = 1;
    };

} // namespace db
