#include "condition_parser.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace db::detail
{

    namespace
    {

        std::optional<CompareOp> compareOpOf(TokenKind kind)
        {
            switch (kind)
            {
                case TokenKind::Equal:
                    return CompareOp::Equal;
                case TokenKind::NotEqual:
                    return CompareOp::NotEqual;
                case TokenKind::Less:
                    return CompareOp::Less;
                case TokenKind::LessEqual:
                    return CompareOp::LessEqual;
                case TokenKind::Greater:
                    return CompareOp::Greater;
                case TokenKind::GreaterEqual:
                    return CompareOp::GreaterEqual;
                default:
                    return std::nullopt;
            }
        }

        // Recursive descent over the grammar documented at parseCondition();
        // one function per rule.
        //
        // As in the RA parser (ra_parser.cpp), each rule fills a node `out` in
        // place and returns its height (the levels it adds, limits.h). A
        // function that returned Conditions by value would keep several of
        // them in its stack frame, and the frames of every level on the
        // current path are live at once. So the functions on the recursion
        // path hold only references and ints, and nodes are built by the
        // attach/begin helpers and parseComparison, which return before the
        // parser recurses.
        //
        // depth_ is the number of levels above the node being parsed: the
        // caller's levels (startDepth), the and/or/not nodes already known to
        // be ancestors, and the open `(`. It never exceeds kMaxNestingDepth.
        class ConditionParser
        {
            // Condition::andOf or Condition::orOf.
            // a function-pointer type,` a pointer to a function that takes two
            // Conditions as arguments and returns a Condition
            using MakeBinary = Condition (*)(Condition, Condition);

        public:
            ConditionParser(const std::vector<Token>& tokens, std::size_t& position, int startDepth)
                : tokens_(tokens),
                  position_(position),
                  depth_(startDepth)
            {
            }

            int parseOr(Condition& out)
            {
                int height = parseAnd(out);
                while (peek().kind == TokenKind::Or)
                {
                    Condition& right = attachRight(out, height, &Condition::orOf);
                    const int rightHeight = parseAnd(right);
                    --depth_;
                    height = 1 + std::max(height, rightHeight);
                }
                return height;
            }

        private:
            const Token& peek() const
            {
                return tokens_[position_];
            }

            // Consumes the current token if it has this kind. End is never
            // consumed, so the cursor stays inside the vector.
            bool match(TokenKind kind)
            {
                if (peek().kind != kind || kind == TokenKind::End)
                {
                    return false;
                }
                ++position_;
                return true;
            }

            [[noreturn]] void throwTooDeep() const
            {
                const Token& token = peek();
                throw Error(ErrorCategory::Syntax,
                            "Condition nested deeper than " + std::to_string(kMaxNestingDepth) + " levels.",
                            token.line, token.column);
            }

            // Called with the `(` or `not` token about to be consumed.
            void enterNesting()
            {
                if (depth_ == kMaxNestingDepth)
                {
                    throwTooDeep();
                }
                ++depth_;
            }

            // --- Rules (on the recursion path) ------------------------------

            int parseAnd(Condition& out)
            {
                int height = parseNot(out);
                while (peek().kind == TokenKind::And)
                {
                    Condition& right = attachRight(out, height, &Condition::andOf);
                    const int rightHeight = parseNot(right);
                    --depth_;
                    height = 1 + std::max(height, rightHeight);
                }
                return height;
            }

            int parseNot(Condition& out)
            {
                if (peek().kind == TokenKind::Not)
                {
                    Condition& operand = beginNot(out);
                    const int height = parseNot(operand);
                    --depth_;
                    return 1 + height;
                }
                return parsePrimary(out);
            }

            int parsePrimary(Condition& out)
            {
                if (peek().kind == TokenKind::LeftParen)
                {
                    enterNesting();
                    ++position_;
                    // Parentheses group; they add no node, so no height.
                    const int height = parseOr(out);
                    if (!match(TokenKind::RightParen))
                    {
                        throwSyntaxError(peek(), "Expected ')' after condition");
                    }
                    --depth_;
                    return height;
                }
                parseComparison(out);
                return 0;
            }

            // --- Node builders (return before the parser recurses) ----------

            // At an `and`/`or` whose left operand `node` is `leftHeight`
            // levels high: the new node puts one more level above it, which
            // must fit. Consumes the operator, turns `node` into
            // `make(node, placeholder)`, enters the new node's level (the
            // caller leaves it after the right operand) and returns the
            // placeholder for the caller to fill.
            Condition& attachRight(Condition& node, int leftHeight, MakeBinary make)
            {
                if (depth_ + 1 + leftHeight > kMaxNestingDepth)
                {
                    throwTooDeep();
                }
                ++position_;
                node = make(std::move(node), Condition());
                ++depth_;
                return node.children[1];
            }

            // At a `not`: enters its level (the caller leaves it), consumes
            // it, turns `out` into `not placeholder` and returns the
            // placeholder.
            Condition& beginNot(Condition& out)
            {
                enterNesting();
                ++position_;
                out = Condition::notOf(Condition());
                return out.children[0];
            }

            void parseComparison(Condition& out)
            {
                Operand left = parseOperand();
                const std::optional<CompareOp> op = compareOpOf(peek().kind);
                if (!op)
                {
                    throwSyntaxError(peek(), "Expected comparison operator");
                }
                ++position_;
                Operand right = parseOperand();
                out = Condition::compare(std::move(left), *op, std::move(right));
            }

            Operand parseOperand()
            {
                const Token& token = peek();
                if (token.kind == TokenKind::Integer)
                {
                    ++position_;
                    return Value::ofInt(token.intValue);
                }
                if (token.kind == TokenKind::String)
                {
                    ++position_;
                    return Value::ofVarchar(token.stringValue);
                }
                if (isNameToken(token.kind))
                {
                    return parseColumnRef();
                }
                throwSyntaxError(token, "Expected a column, integer or string");
            }

            ColumnRef parseColumnRef()
            {
                ColumnRef ref;
                ref.name = tokens_[position_++].text;
                if (match(TokenKind::Dot))
                {
                    if (!isNameToken(peek().kind))
                    {
                        throwSyntaxError(peek(), "Expected column name after '.'");
                    }
                    ref.qualifier = std::move(ref.name);
                    ref.name = tokens_[position_++].text;
                }
                return ref;
            }

            const std::vector<Token>& tokens_;
            std::size_t& position_;
            int depth_;
        };

    } // namespace

    ParsedCondition parseConditionAt(const std::vector<Token>& tokens, std::size_t& position, int startDepth)
    {
        ParsedCondition parsed;
        parsed.height = ConditionParser(tokens, position, startDepth).parseOr(parsed.condition);
        return parsed;
    }

    bool isNameToken(TokenKind kind)
    {
        switch (kind)
        {
            case TokenKind::Identifier:
            case TokenKind::Select:
            case TokenKind::Project:
            case TokenKind::Rename:
            case TokenKind::Join:
            case TokenKind::Union:
            case TokenKind::Intersect:
            case TokenKind::Minus:
            case TokenKind::Times:
            case TokenKind::Distinct:
                return true;
            default:
                return false;
        }
    }

    std::string describeToken(const Token& token)
    {
        if (token.kind == TokenKind::End)
        {
            return "end of input";
        }
        if (token.kind == TokenKind::String)
        {
            return "string " + token.text;
        }
        return "'" + token.text + "'";
    }

    void throwSyntaxError(const Token& token, std::string_view expected)
    {
        throw Error(ErrorCategory::Syntax, std::string(expected) + ", found " + describeToken(token) + ".",
                    token.line, token.column);
    }

} // namespace db::detail
