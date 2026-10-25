//
// Created by amira on 2026-09-27.
//

#include "parser_context.h"

#include "db/common/error.h"

namespace ra
{
    ParserContext::ParserContext(Tokenizer& tokenizer)
        : tokenizer(tokenizer), current(0)
    {
    }

    bool ParserContext::isAtEnd() const
    {
        return peek().type == TokenType::END_OF_FILE;
    }

    const Token& ParserContext::peek() const
    {
        return tokenizer.tokenAt(current);
    }

    const Token& ParserContext::peekAhead(std::size_t offset) const
    {
        return tokenizer.tokenAt(current + offset);
    }

    bool ParserContext::check(TokenType type) const
    {
        return peek().type == type;
    }

    bool ParserContext::match(std::initializer_list<TokenType> types)
    {
        for (TokenType type : types)
        {
            if (check(type))
            {
                advance();
                return true;
            }
        }

        return false;
    }

    Token ParserContext::advance()
    {
        Token token = peek();
        if (!isAtEnd())
            current++;

        return token;
    }

    Token ParserContext::consume(
        TokenType type,
        const std::string& errorMessage)
    {
        if (check(type))
            return advance();

        throwParseError(peek(), errorMessage);
    }

    bool ParserContext::checkAttributeName() const
    {
        return isAttributeNameToken(peek().type);
    }

    bool ParserContext::isAttributeNameToken(TokenType type)
    {
        switch (type)
        {
        case TokenType::IDENTIFIER:
        case TokenType::SELECT:
        case TokenType::PROJECT:
        case TokenType::RENAME:
        case TokenType::UNION:
        case TokenType::INTERSECT:
        case TokenType::MINUS:
        case TokenType::TIMES:
        case TokenType::JOIN:
        case TokenType::DISTINCT:
        case TokenType::AND:
        case TokenType::OR:
        case TokenType::NOT:
            return true;

        default:
            return false;
        }
    }

    std::string ParserContext::consumeAttributeName(
        const std::string& errorMessage)
    {
        if (checkAttributeName())
            return advance().lexeme;

        throwParseError(peek(), errorMessage);
    }

    [[noreturn]]
    void throwParseError(
        const Token& token,
        const std::string& message)
    {
        throw db::Error(
            db::ErrorCategory::Syntax,
            message + " (Got '" + token.lexeme + "')",
            token.line,
            token.column
        );
    }
}
