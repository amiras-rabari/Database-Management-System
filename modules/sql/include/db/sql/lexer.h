#pragma once

/// @file
/// The SQL lexer: SQL text in, a list of tokens with positions out (§M3.1).
/// Hand-written, no regular expressions.

#include <db/sql/source_pos.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace db::sql
{

    /// What a token is. Each keyword has its own kind, so the parser can
    /// say "expect FROM" directly. Any other word is an Identifier.
    enum class SqlTokenKind
    {
        Identifier,
        Integer,
        String,

        // Punctuation: ( ) , . ; *
        LeftParen,
        RightParen,
        Comma,
        Dot,
        Semicolon,
        Star,

        // Comparison operators: = <> < <= > >=
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,

        // Keywords: every upper-case word of the §M3.2 grammar. All reserved.
        Select,
        Distinct,
        From,
        Where,
        Group,
        By,
        Having,
        Order,
        Asc,
        Desc,
        Limit,
        Union,
        All,
        Intersect,
        Except,
        Join,
        On,
        As,
        And,
        Or,
        Not,
        Create,
        Table,
        Insert,
        Into,
        Values,
        Int,
        Varchar,
        Count,
        Sum,
        Avg,
        Min,
        Max,

        /// Always the last token, so the parser can look one token ahead
        /// without running off the end of the list.
        EndOfInput
    };

    /// One token of SQL text.
    struct SqlToken
    {
        SqlTokenKind kind = SqlTokenKind::EndOfInput;
        /// Identifier or keyword: the spelling as written (`Emp`, `select`).
        /// String: the value with each '' turned into one ' (`it's`).
        /// Integer, punctuation, operator: the characters as written.
        /// EndOfInput: empty.
        std::string text;
        /// Position of the token's first character (for a string, its opening
        /// quote; for EndOfInput, just past the last character).
        SourcePos pos;
        /// Integer only: the checked 32-bit value (`-5`, `007` gives 7).
        int32_t intValue = 0;
    };

    /// Splits SQL text (a whole script, any number of statements) into
    /// tokens. Whitespace and `--` comments produce no tokens. The list
    /// always ends with one EndOfInput token.
    ///
    /// Throws db::Error(Lexical) at the position of the first bad input:
    /// an unexpected character (`#`, `"`, `!`), a sign not followed directly
    /// by a digit, digits running into a letter (`12abc`), an integer outside
    /// the int32 range, or a string with no closing quote.
    std::vector<SqlToken> tokenizeSql(std::string_view text);

} // namespace db::sql
