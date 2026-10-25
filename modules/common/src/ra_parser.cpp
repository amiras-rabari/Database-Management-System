#include "db/common/ra_parser.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "condition_parser.h"
#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/tokenizer.h"

namespace db
{

    namespace
    {

        // Recursive descent over the grammar documented at parseRaQuery();
        // one parse function per rule, ported from Bonus 1's QueryParser.
        //
        // Each parse function fills a node `out` in place and returns its
        // height (the levels it adds, limits.h), instead of returning a node.
        // A PlanNode is a few hundred bytes, and a function that returns
        // nodes by value keeps several of them (operands, arguments, results)
        // in its stack frame; the frames of every level on the current path
        // are live at once. So the functions on the recursion path hold only
        // references and ints, and nodes are built by the small attach/begin
        // helpers, which return before the parser recurses.
        //
        // depth_ is the number of levels above the node being parsed: the
        // operator nodes already known to be its ancestors and the open `(`.
        // It never exceeds kMaxNestingDepth. A condition continues the same
        // count from the select or join node that holds it.
        class RaParser
        {
            // A named constructor of a binary node without a condition.
            using MakeBinary = PlanNode (*)(PlanNode, PlanNode);

        public:
            explicit RaParser(const std::vector<Token>& tokens)
                : tokens_(tokens)
            {
            }

            PlanNode parseQuery()
            {
                PlanNode plan;
                parseExpression(plan);
                if (peek().kind != TokenKind::End)
                {
                    detail::throwSyntaxError(peek(), "Expected end of query");
                }
                return plan;
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

            void expect(TokenKind kind, std::string_view expected)
            {
                if (!match(kind))
                {
                    detail::throwSyntaxError(peek(), expected);
                }
            }

            [[noreturn]] void throwTooDeep() const
            {
                const Token& token = peek();
                throw Error(ErrorCategory::Syntax,
                            "Query nested deeper than " + std::to_string(kMaxNestingDepth) + " levels.", token.line,
                            token.column);
            }

            // Called with the `(` or unary keyword about to be consumed.
            void enterNesting()
            {
                if (depth_ == kMaxNestingDepth)
                {
                    throwTooDeep();
                }
                ++depth_;
            }

            // --- Rules (on the recursion path) ------------------------------
            //
            // A binary operator loop keeps `height` as the height of the tree
            // built so far; attach* makes it the new node's height without
            // its right operand, and the right operand can only raise it.

            int parseExpression(PlanNode& out)
            {
                // Bonus 1's hint for two relation names in a row (`A B`).
                if (peek().kind == TokenKind::Identifier && tokens_[position_ + 1].kind == TokenKind::Identifier)
                {
                    detail::throwSyntaxError(tokens_[position_ + 1],
                                             "Expected a binary operator between expressions");
                }
                int height = parseIntersect(out);
                while (peek().kind == TokenKind::Union || peek().kind == TokenKind::Minus)
                {
                    const MakeBinary make = peek().kind == TokenKind::Union ? &PlanNode::unionOf : &PlanNode::except;
                    PlanNode& right = attachRight(out, height, make);
                    height = std::max(height, 1 + parseIntersect(right));
                    --depth_;
                }
                return height;
            }

            int parseIntersect(PlanNode& out)
            {
                int height = parseCombine(out);
                while (peek().kind == TokenKind::Intersect)
                {
                    PlanNode& right = attachRight(out, height, &PlanNode::intersect);
                    height = std::max(height, 1 + parseCombine(right));
                    --depth_;
                }
                return height;
            }

            int parseCombine(PlanNode& out)
            {
                int height = parseUnary(out);
                while (peek().kind == TokenKind::Times || peek().kind == TokenKind::Join)
                {
                    PlanNode& right = peek().kind == TokenKind::Times ? attachRight(out, height, &PlanNode::cross)
                                                                      : attachJoinRight(out, height);
                    height = std::max(height, 1 + parseUnary(right));
                    --depth_;
                }
                return height;
            }

            int parseUnary(PlanNode& out)
            {
                // Set by the begin helper: the node's height without its child.
                int height = 0;
                switch (peek().kind)
                {
                    case TokenKind::Select:
                    {
                        PlanNode& child = beginSelect(out, height);
                        return std::max(height, 1 + parseOperand(child, "Expected '(' after select condition",
                                                                 "Expected ')' after select expression"));
                    }
                    case TokenKind::Project:
                    {
                        PlanNode& child = beginProject(out, height);
                        return std::max(height, 1 + parseOperand(child, "Expected '(' after projection list",
                                                                 "Expected ')' after project expression"));
                    }
                    case TokenKind::Rename:
                    {
                        PlanNode& child = beginRename(out, height);
                        return std::max(height, 1 + parseOperand(child, "Expected '(' after rename name",
                                                                 "Expected ')' after rename expression"));
                    }
                    case TokenKind::Distinct:
                    {
                        PlanNode& child = beginDistinct(out, height);
                        return std::max(height, 1 + parseOperand(child, "Expected '(' after 'distinct'",
                                                                 "Expected ')' after distinct expression"));
                    }
                    default:
                        return parsePrimary(out);
                }
            }

            // The `(expression)` every unary operator ends with; leaves the
            // level its begin helper entered.
            int parseOperand(PlanNode& out, std::string_view expectedOpen, std::string_view expectedClose)
            {
                expect(TokenKind::LeftParen, expectedOpen);
                const int height = parseExpression(out);
                expect(TokenKind::RightParen, expectedClose);
                --depth_;
                return height;
            }

            int parsePrimary(PlanNode& out)
            {
                if (peek().kind == TokenKind::LeftParen)
                {
                    enterNesting();
                    ++position_;
                    // Parentheses group; they add no node, so no height.
                    const int height = parseExpression(out);
                    expect(TokenKind::RightParen, "Expected ')' after expression");
                    --depth_;
                    return height;
                }
                readScan(out);
                return 0;
            }

            // --- Node builders (return before the parser recurses) ----------

            // At a binary operator whose left operand `node` is `height`
            // levels high: the new node puts one more level above it, which
            // must fit. Consumes the operator, turns `node` into
            // `make(node, placeholder)`, sets `height` to the new node's
            // height so far, enters the new node's level (the caller leaves
            // it after the right operand) and returns the placeholder for the
            // caller to fill.
            PlanNode& attachRight(PlanNode& node, int& height, MakeBinary make)
            {
                if (depth_ + 1 + height > kMaxNestingDepth)
                {
                    throwTooDeep();
                }
                ++position_;
                node = make(std::move(node), PlanNode());
                ++height;
                ++depth_;
                return node.children[1];
            }

            // attachRight for `join [condition]`; the condition's levels
            // start below the join node.
            PlanNode& attachJoinRight(PlanNode& node, int& height)
            {
                if (depth_ + 1 + height > kMaxNestingDepth)
                {
                    throwTooDeep();
                }
                ++position_;
                ++depth_;
                expect(TokenKind::LeftBracket, "Expected '[' after 'join'");
                detail::ParsedCondition parsed = detail::parseConditionAt(tokens_, position_, depth_);
                expect(TokenKind::RightBracket, "Expected ']' after join condition");
                node = PlanNode::join(std::move(parsed.condition), std::move(node), PlanNode());
                height = 1 + std::max(height, parsed.height);
                return node.children[1];
            }

            // Each begin helper enters the operator's level (parseOperand
            // leaves it), reads the operator up to its `(`, turns `out` into
            // that operator over a placeholder child, sets `height` to the
            // node's height without the child and returns the placeholder.

            PlanNode& beginSelect(PlanNode& out, int& height)
            {
                enterNesting();
                ++position_;
                expect(TokenKind::LeftBracket, "Expected '[' after 'select'");
                detail::ParsedCondition parsed = detail::parseConditionAt(tokens_, position_, depth_);
                expect(TokenKind::RightBracket, "Expected ']' after select condition");
                out = PlanNode::select(std::move(parsed.condition), PlanNode());
                height = 1 + parsed.height;
                return out.children[0];
            }

            PlanNode& beginProject(PlanNode& out, int& height)
            {
                enterNesting();
                ++position_;
                expect(TokenKind::LeftBracket, "Expected '[' after 'project'");
                std::vector<ColumnRef> columns;
                columns.push_back(parseColumnRef("Expected at least one column in the projection list"));
                while (match(TokenKind::Comma))
                {
                    columns.push_back(parseColumnRef("Expected a column after ','"));
                }
                expect(TokenKind::RightBracket, "Expected ']' after projection list");
                out = PlanNode::project(std::move(columns), PlanNode());
                height = 1;
                return out.children[0];
            }

            PlanNode& beginRename(PlanNode& out, int& height)
            {
                enterNesting();
                ++position_;
                expect(TokenKind::LeftBracket, "Expected '[' after 'rename'");
                if (peek().kind != TokenKind::Identifier)
                {
                    detail::throwSyntaxError(peek(), "Expected a relation name inside rename");
                }
                std::string alias = tokens_[position_++].text;
                expect(TokenKind::RightBracket, "Expected ']' after rename name");
                out = PlanNode::rename(std::move(alias), PlanNode());
                height = 1;
                return out.children[0];
            }

            PlanNode& beginDistinct(PlanNode& out, int& height)
            {
                enterNesting();
                ++position_;
                out = PlanNode::distinct(PlanNode());
                height = 1;
                return out.children[0];
            }

            void readScan(PlanNode& out)
            {
                if (peek().kind != TokenKind::Identifier)
                {
                    detail::throwSyntaxError(peek(), "Expected relation name or '('");
                }
                out = PlanNode::scan(tokens_[position_++].text);
            }

            // Same rule as a column in a condition: name [ "." name ].
            ColumnRef parseColumnRef(std::string_view expected)
            {
                if (!detail::isNameToken(peek().kind))
                {
                    detail::throwSyntaxError(peek(), expected);
                }
                ColumnRef ref;
                ref.name = tokens_[position_++].text;
                if (match(TokenKind::Dot))
                {
                    if (!detail::isNameToken(peek().kind))
                    {
                        detail::throwSyntaxError(peek(), "Expected column name after '.'");
                    }
                    ref.qualifier = std::move(ref.name);
                    ref.name = tokens_[position_++].text;
                }
                return ref;
            }


            //// A difference from the condition parser: there, position_ was a reference to someone
            ///else’s cursor. Here the RA parser is the top-level parser, so it owns the cursor itself.
            ///It later lends it out to the condition parser.
            const std::vector<Token>& tokens_;
            std::size_t position_ = 0;
            int depth_ = 0;
        };

    } // namespace

    PlanNode parseRaQuery(std::string_view text)
    {
        const std::vector<Token> tokens = tokenize(text);
        return RaParser(tokens).parseQuery();
    }

} // namespace db
