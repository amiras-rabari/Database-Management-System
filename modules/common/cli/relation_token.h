//
// Created by amira on 2026-09-22.
//

#pragma once

/// @file
/// Tokens of the Bonus 1 relation-text lexer (CLI only).

#include <cstddef>
#include <string>
#include <variant>

namespace ra
{
    // 1. Strongly typed enum for fast switch statements and checks
    enum class TokenType
    {
        // Keywords
        SELECT, PROJECT, RENAME, UNION, INTERSECT, MINUS, TIMES, JOIN, DISTINCT,
        AND, OR, NOT,

        // Dynamic Tokens
        IDENTIFIER,
        INTEGER,
        QUOTED_STRING,
        BARE_STRING,

        // Operators
        EQUAL, // =
        NOT_EQUAL, // !=
        LESS, // <
        LESS_EQUAL, // <=
        GREATER, // >
        GREATER_EQUAL, // >=

        // Punctuation
        LPAREN, RPAREN, // ( )
        LBRACKET, RBRACKET, // [ ]
        LBRACE, RBRACE, // { }
        DOT, COMMA, // . ,

        // Control
        NEWLINE,
        END_OF_FILE
    };

    // 2. Token record structure
    struct Token
    {
        TokenType type; // What kind of token is this?
        std::string lexeme; // Exact text from input (e.g., "-30")

        // The Payload (handles your variations):
        // std::monostate means "no value" (used for keywords/operators).
        // std::string stores decoded strings and the complete integer text,
        // so integer literals are not limited by the range of int.
        std::variant<std::monostate, std::string> value;
        int line; // For error reporting
        int column; // For error reporting
        std::size_t offset; // Byte offset of the first character in the input
    };

} // namespace ra
