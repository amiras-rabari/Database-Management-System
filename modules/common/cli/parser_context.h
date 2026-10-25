#pragma once

/// @file
/// Token cursor shared by the relation-text parser (CLI only).

#include "relation_lexer.h"
#include "relation_token.h"

#include <cstddef>
#include <initializer_list>
#include <string>

namespace ra
{
    /// Reads tokens from a Tokenizer on demand, so lookahead lexes only as
    /// far as it looks.
    class ParserContext
    {
    public:
        /// Reads from `tokenizer`, which must outlive the context.
        explicit ParserContext(Tokenizer& tokenizer);

        bool isAtEnd() const;

        const Token& peek() const;
        const Token& peekAhead(std::size_t offset) const;

        bool check(TokenType type) const;
        bool match(std::initializer_list<TokenType> types);

        Token advance();
        Token consume(
            TokenType type,
            const std::string& errorMessage
        );

        bool checkAttributeName() const;

        static bool isAttributeNameToken(TokenType type);

        std::string consumeAttributeName(
            const std::string& errorMessage
        );

    private:
        Tokenizer& tokenizer;
        std::size_t current = 0;
    };

    /// Throws db::Error(Syntax) at `token`'s position.
    [[noreturn]]
    void throwParseError(
        const Token& token,
        const std::string& message
    );
}
