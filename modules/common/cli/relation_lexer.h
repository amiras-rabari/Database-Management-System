//
// Created by amira on 2026-09-22.
//

#pragma once

/// @file
/// Lexer for Bonus 1 relation text (CLI only). Outside `{ }` it reads the
/// query-like tokens a relation header uses; inside `{ }` it reads row
/// values and newlines.

#include "relation_token.h"

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace ra
{
    /// Turns text into tokens on demand: tokenAt(i) reads only as far as
    /// token i, so the relation parser never lexes more of the query that
    /// follows the relations than its lookahead needs.
    class Tokenizer {
    private:
        std::string source;
        // A deque, so references returned by tokenAt stay valid while more
        // tokens are read.
        std::deque<Token> tokens;
        bool finished = false;

        std::size_t start_index = 0;
        std::size_t current_index = 0;

        int line = 1;
        int current_column = 1;
        int start_column = 1;

        bool in_relation_data = false; // Toggled by { and }

        // Keyword lookup table
        const std::map<std::string, TokenType> keywords;

        // --- Core Navigation Helpers ---
        //A const keyword placed at the end of a C++
        //member function means that the method is a read-only function
        //that cannot modify the object's member variables
        bool isAtEnd() const;
        char peek() const;
        char peekNext() const;
        char advance();
        bool match(char expected);

        // --- Token Producers ---
        void addToken(TokenType type);
        void addToken(TokenType type, std::variant<std::monostate, std::string> literal);
        [[noreturn]] void throwLexicalError(const std::string& message);
        void skipWhitespaceAndComments();
        void parseString();
        void parseNumber();
        void parseDigitStartedToken();
        void parseIdentifierOrKeyword();
        void parseRelationDataValue();

        // --- Main Loop ---
        void scanToken();

    public:
        explicit Tokenizer(const std::string& input);

        /// Token `index` (0-based), lexing more input if needed. Past the end
        /// of the input this is the END_OF_FILE token.
        /// Throws db::Error(Lexical) for invalid input.
        const Token& tokenAt(std::size_t index);

        /// Every token of the input, ending with END_OF_FILE.
        std::vector<Token> tokenizeAll();
    };
} // namespace ra
