// The SQL parser: hand-written recursive descent over the lexer's tokens
// (§M3.2). Each grammar rule is one method. Precedence and associativity
// come from how the methods call each other, never from a precedence table.
// First error only (§M3.6): the first unexpected token throws, and the
// exception ends the whole parse.

#include "parser/sql_parser.h"

#include <db/sql/parser.h>

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/limits.h>
#include <db/common/plan.h>
#include <db/common/value.h>
#include <db/sql/ast.h>
#include <db/sql/lexer.h>
#include <db/sql/source_pos.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace db::sql
{

    namespace
    {

        class SqlParser
        {
        public:
            explicit SqlParser(std::vector<SqlToken> tokens) : tokens_(std::move(tokens)) {}

            // condition ::= or-level. The entry point for WHERE, ON, HAVING.
            SqlCondition parseCondition()
            {
                SqlCondition condition;
                parseOr(condition);
                return condition;
            }

            // Requires that every token was used, e.g. nothing after a condition.
            void expectEnd()
            {
                if (peek().kind != SqlTokenKind::EndOfInput)
                {
                    throw syntaxError("end of input");
                }
            }

            // script ::= { statement } with statements separated by ';'.
            // Empty statements (";;", only comments) are skipped and the final
            // ';' is optional, because the shell sends single statements,
            // usually without one (§M3.1: "A file may hold several").
            std::vector<Statement> parseScript()
            {
                std::vector<Statement> statements;
                while (true)
                {
                    while (accept(SqlTokenKind::Semicolon))
                    {
                    }
                    if (peek().kind == SqlTokenKind::EndOfInput)
                    {
                        return statements;
                    }
                    // Parsed in place into its vector slot; the vector does not
                    // grow again until this statement is finished.
                    parseStatement(statements.emplace_back());
                    expectStatementEnd();
                }
            }

            // Exactly one query, optionally followed by ';'.
            QueryExpr parseOneQuery()
            {
                QueryExpr query;
                parseQuery(query);
                expectStatementEnd();
                accept(SqlTokenKind::Semicolon);
                expectEnd();
                return query;
            }

        private:
            // Counts how deep the parse is nested. Every '(' (in a condition
            // or around a query), FROM subquery, NOT, AND, OR and set operator
            // adds one level, the same budget common uses for its parsers
            // (kMaxNestingDepth, DECISIONS "Nesting limit"). Without it, very
            // deep input would overflow the stack, since each '(' or NOT is
            // one more recursive call.
            //
            // RAII: the guard gives back every level it added in its
            // destructor, so the count stays right on every way out of a
            // method, including an exception. Not copyable: a copy would give
            // the levels back twice.
            class DepthGuard
            {
            public:
                explicit DepthGuard(SqlParser& parser) : parser_(parser) {}
                DepthGuard(const DepthGuard&) = delete;
                DepthGuard& operator=(const DepthGuard&) = delete;
                ~DepthGuard()
                {
                    parser_.depth_ -= added_;
                }

                // One level deeper. Throws at the current token if that goes
                // past the budget.
                void enter()
                {
                    ++parser_.depth_;
                    ++added_;
                    if (parser_.depth_ > kMaxNestingDepth)
                    {
                        const SourcePos pos = parser_.peek().pos;
                        throw Error(ErrorCategory::Syntax, "nesting too deep", pos.line, pos.column);
                    }
                }

            private:
                SqlParser& parser_;
                int added_ = 0;
            };

            // --- Token cursor ------------------------------------------------
            // All token access goes through these, so every error gets the
            // position of the token it is about. The lexer always ends the
            // list with EndOfInput, and nothing advances past it, so peek()
            // is always valid.

            // The current token.
            const SqlToken& peek() const
            {
                return tokens_[index_];
            }

            // Returns the current token and moves to the next one.
            const SqlToken& advance()
            {
                const SqlToken& token = tokens_[index_];
                if (token.kind != SqlTokenKind::EndOfInput)
                {
                    ++index_;
                }
                return token;
            }

            // Consumes the current token if it has this kind.
            bool accept(SqlTokenKind kind)
            {
                if (peek().kind == kind)
                {
                    advance();
                    return true;
                }
                return false;
            }

            // Consumes a token of this kind, or throws "expected <what> but
            // found ..." at the current token.
            const SqlToken& expect(SqlTokenKind kind, std::string_view what)
            {
                if (peek().kind != kind)
                {
                    throw syntaxError(what);
                }
                return advance();
            }

            // "expected <what> but found '<token>'" at the current token, the
            // one the parser could not use.
            Error syntaxError(std::string_view what) const
            {
                const SqlToken& token = peek();
                const std::string found =
                    token.kind == SqlTokenKind::EndOfInput ? "end of input" : "'" + token.text + "'";
                return Error(ErrorCategory::Syntax, "expected " + std::string(what) + " but found " + found,
                             token.pos.line, token.pos.column);
            }

            // --- Columns and operands ---------------------------------------

            // column ::= ident | ident "." ident
            ColumnName parseColumn()
            {
                const SqlToken& first = expect(SqlTokenKind::Identifier, "a column name");
                ColumnName column{std::nullopt, first.text, first.pos};
                if (accept(SqlTokenKind::Dot))
                {
                    const SqlToken& second = expect(SqlTokenKind::Identifier, "a column name after '.'");
                    column.qualifier = column.name;
                    column.name = second.text;
                }
                return column;
            }

            // The aggregate function of a keyword token, or nothing.
            static std::optional<AggregateFunction> aggregateFunctionOf(SqlTokenKind kind)
            {
                switch (kind)
                {
                case SqlTokenKind::Count:
                    return AggregateFunction::Count;
                case SqlTokenKind::Sum:
                    return AggregateFunction::Sum;
                case SqlTokenKind::Avg:
                    return AggregateFunction::Avg;
                case SqlTokenKind::Min:
                    return AggregateFunction::Min;
                case SqlTokenKind::Max:
                    return AggregateFunction::Max;
                default:
                    return std::nullopt;
                }
            }

            // aggregate ::= ( COUNT | SUM | AVG | MIN | MAX ) "(" ( column | "*" ) ")"
            // The caller checked that the current token is one of the five.
            // `*` is accepted for every function here; the checker (chunk 08)
            // rejects SUM(*) and the others as a type error.
            AggregateCall parseAggregateCall()
            {
                const SqlToken& name = advance();
                AggregateCall call{*aggregateFunctionOf(name.kind), std::nullopt, name.pos};
                expect(SqlTokenKind::LeftParen, "'('");
                if (!accept(SqlTokenKind::Star))
                {
                    if (peek().kind != SqlTokenKind::Identifier)
                    {
                        throw syntaxError("a column or '*'");
                    }
                    call.argument = parseColumn();
                }
                expect(SqlTokenKind::RightParen, "')'");
                return call;
            }

            // literal ::= int | string. INT for an integer token, VARCHAR for a
            // string token (already unescaped by the lexer).
            Literal parseLiteral()
            {
                const SqlToken& token = peek();
                if (token.kind == SqlTokenKind::Integer)
                {
                    advance();
                    return Literal{Value::ofInt(token.intValue), token.pos};
                }
                if (token.kind == SqlTokenKind::String)
                {
                    advance();
                    return Literal{Value::ofVarchar(token.text), token.pos};
                }
                throw syntaxError("a literal (an integer or a string)");
            }

            // operand ::= column | int | string, plus an aggregate call
            // (HAVING compares aggregates; an aggregate in WHERE is rejected
            // later with an `aggregate` error, required case 7).
            Operand parseOperand()
            {
                const SqlTokenKind kind = peek().kind;
                if (kind == SqlTokenKind::Integer || kind == SqlTokenKind::String)
                {
                    return parseLiteral();
                }
                if (kind == SqlTokenKind::Identifier)
                {
                    return parseColumn();
                }
                if (aggregateFunctionOf(kind))
                {
                    return parseAggregateCall();
                }
                throw syntaxError("an operand");
            }

            // A name as written: an identifier token turned into a Name.
            Name parseName(std::string_view what)
            {
                const SqlToken& token = expect(SqlTokenKind::Identifier, what);
                return Name{token.text, token.pos};
            }

            // The comparison operator of a token, or nothing.
            static std::optional<CompareOp> compareOpOf(SqlTokenKind kind)
            {
                switch (kind)
                {
                case SqlTokenKind::Equal:
                    return CompareOp::Equal;
                case SqlTokenKind::NotEqual:
                    return CompareOp::NotEqual;
                case SqlTokenKind::Less:
                    return CompareOp::Less;
                case SqlTokenKind::LessEqual:
                    return CompareOp::LessEqual;
                case SqlTokenKind::Greater:
                    return CompareOp::Greater;
                case SqlTokenKind::GreaterEqual:
                    return CompareOp::GreaterEqual;
                default:
                    return std::nullopt;
                }
            }

            // --- Condition levels -------------------------------------------
            // Four methods, each calling the next tighter one:
            //   parseOr -> parseAnd -> parseNot -> parsePrimary.
            // So in `a OR b AND NOT c` the AND is built inside an OR operand
            // and the NOT inside an AND operand: Or(a, And(b, Not(c))).
            // That is NOT > AND > OR (§M3.2), with no precedence numbers.

            // Stack size: these four methods recurse once per '(' or NOT, and
            // 1000 levels must work (kMaxNestingDepth). A SqlCondition is
            // about 240 bytes, so keeping one in each frame would overflow
            // the stack below 1000 levels. Instead each method fills a node
            // the caller owns (`out`, a fresh default node), and a child is
            // parsed straight into its slot of the parent's `children`
            // vector, which lives on the heap. Frames hold only small things.

            // Turns `out` into a binary And/Or node: the condition already in
            // `out` becomes the left child, and an empty right child is added
            // for the caller to fill. The node's position is the left child's,
            // i.e. where the whole expression starts.
            static void makeBinary(ConditionKind kind, SqlCondition& out)
            {
                std::vector<SqlCondition> children(2);
                children[0] = std::move(out);
                out.kind = kind;
                out.op = CompareOp::Equal;
                // Reset the operands left behind by the move, so the node
                // equals one built by hand (And/Or nodes do not use them).
                out.left.emplace<ColumnName>();
                out.right.emplace<ColumnName>();
                out.pos = children[0].pos;
                out.children = std::move(children);
            }

            // or-level ::= and-level { OR and-level }
            // The loop folds to the left: a OR b OR c is Or(Or(a, b), c),
            // the same shape common's orOf and parseCondition build.
            void parseOr(SqlCondition& out)
            {
                DepthGuard guard(*this);
                parseAnd(out);
                while (peek().kind == SqlTokenKind::Or)
                {
                    guard.enter(); // the left spine is one Or node deeper
                    advance();
                    makeBinary(ConditionKind::Or, out);
                    parseAnd(out.children[1]);
                }
            }

            // and-level ::= not-level { AND not-level }, folded to the left.
            void parseAnd(SqlCondition& out)
            {
                DepthGuard guard(*this);
                parseNot(out);
                while (peek().kind == SqlTokenKind::And)
                {
                    guard.enter();
                    advance();
                    makeBinary(ConditionKind::And, out);
                    parseNot(out.children[1]);
                }
            }

            // not-level ::= NOT not-level | primary
            // NOT calls itself, so NOT NOT a = 1 is Not(Not(a = 1)).
            void parseNot(SqlCondition& out)
            {
                if (peek().kind != SqlTokenKind::Not)
                {
                    parsePrimary(out);
                    return;
                }
                DepthGuard guard(*this);
                guard.enter();
                out.kind = ConditionKind::Not;
                out.pos = advance().pos;
                out.children.resize(1);
                parseNot(out.children[0]);
            }

            // primary ::= "(" condition ")" | comparison
            // An operand never starts with '(', so a '(' here always opens a
            // nested condition: one token decides, no backtracking.
            void parsePrimary(SqlCondition& out)
            {
                if (peek().kind != SqlTokenKind::LeftParen)
                {
                    parseComparison(out);
                    return;
                }
                DepthGuard guard(*this);
                guard.enter();
                advance();
                parseOr(out);
                expect(SqlTokenKind::RightParen, "')'");
            }

            // comparison ::= operand compare-op operand
            // Not recursive, so its locals never pile up on the stack.
            void parseComparison(SqlCondition& out)
            {
                out.kind = ConditionKind::Compare;
                out.pos = peek().pos;
                out.left = parseOperand();
                const std::optional<CompareOp> op = compareOpOf(peek().kind);
                if (!op)
                {
                    throw syntaxError("a comparison operator (=, <>, <, <=, >, >=)");
                }
                advance();
                out.op = *op;
                out.right = parseOperand();
            }

            // --- Statements -------------------------------------------------
            // Same in-place style as the conditions: each method fills a node
            // the caller owns, so the large SelectBlock never sits in a stack
            // frame while the parser recurses into a subquery.

            // After a statement: ';' or end of input. Anything else is a
            // clause out of order, a mixed FROM form, a second VALUES row, or
            // a missing ';' between two statements.
            void expectStatementEnd()
            {
                const SqlTokenKind kind = peek().kind;
                if (kind != SqlTokenKind::Semicolon && kind != SqlTokenKind::EndOfInput)
                {
                    throw syntaxError("';' or end of input");
                }
            }

            // statement ::= create | insert | query
            // The first token decides: CREATE, INSERT, or SELECT / '(' for a
            // query.
            void parseStatement(Statement& out)
            {
                switch (peek().kind)
                {
                case SqlTokenKind::Create:
                    parseCreate(out.emplace<CreateTableStmt>());
                    return;
                case SqlTokenKind::Insert:
                    parseInsert(out.emplace<InsertStmt>());
                    return;
                case SqlTokenKind::Select:
                case SqlTokenKind::LeftParen:
                    parseQuery(out.emplace<QueryExpr>());
                    return;
                default:
                    throw syntaxError("a statement (SELECT, CREATE TABLE or INSERT)");
                }
            }

            // create ::= CREATE TABLE ident "(" coldef { "," coldef } ")"
            // coldef ::= ident ( INT | VARCHAR "(" int ")" )
            // The VARCHAR length is kept as written; its range (1..255) is a
            // schema rule, checked by common's Schema in chunk 11.
            void parseCreate(CreateTableStmt& out)
            {
                advance(); // CREATE
                expect(SqlTokenKind::Table, "TABLE");
                out.table = parseName("a table name");
                expect(SqlTokenKind::LeftParen, "'('");
                do
                {
                    ColumnDef& column = out.columns.emplace_back();
                    column.name = parseName("a column name");
                    if (accept(SqlTokenKind::Int))
                    {
                        column.type = Type::Int;
                    }
                    else if (accept(SqlTokenKind::Varchar))
                    {
                        column.type = Type::Varchar;
                        expect(SqlTokenKind::LeftParen, "'('");
                        column.varcharLength = expect(SqlTokenKind::Integer, "an integer").intValue;
                        expect(SqlTokenKind::RightParen, "')'");
                    }
                    else
                    {
                        throw syntaxError("INT or VARCHAR");
                    }
                } while (accept(SqlTokenKind::Comma));
                expect(SqlTokenKind::RightParen, "')'");
            }

            // insert ::= INSERT INTO ident VALUES "(" literal { "," literal } ")"
            // One row only: a second "(...)" is left over and fails at the
            // statement end.
            void parseInsert(InsertStmt& out)
            {
                advance(); // INSERT
                expect(SqlTokenKind::Into, "INTO");
                out.table = parseName("a table name");
                expect(SqlTokenKind::Values, "VALUES");
                expect(SqlTokenKind::LeftParen, "'('");
                do
                {
                    out.values.push_back(parseLiteral());
                } while (accept(SqlTokenKind::Comma));
                expect(SqlTokenKind::RightParen, "')'");
            }

            // --- Queries ----------------------------------------------------

            // The set operator a token starts, or nothing. UNION ALL is told
            // apart from UNION after the UNION token is consumed.
            static std::optional<SetOpKind> setOpKindOf(SqlTokenKind kind)
            {
                switch (kind)
                {
                case SqlTokenKind::Union:
                    return SetOpKind::Union;
                case SqlTokenKind::Intersect:
                    return SetOpKind::Intersect;
                case SqlTokenKind::Except:
                    return SetOpKind::Except;
                default:
                    return std::nullopt;
                }
            }

            // Turns `out` into a set operation: the query already in `out`
            // becomes the left operand, and an empty right operand is added
            // for the caller to fill (the same idea as makeBinary).
            static void makeSetOp(SetOpKind kind, SourcePos pos, QueryExpr& out)
            {
                std::vector<QueryExpr> operands(2);
                operands[0] = std::move(out);
                SetOperation& operation = out.node.emplace<SetOperation>();
                operation.kind = kind;
                operation.pos = pos;
                operation.operands = std::move(operands);
            }

            // query ::= select_block { setop select_block }
            // setop ::= UNION [ ALL ] | INTERSECT | EXCEPT
            // One loop for all three operators, folding to the left, so
            // A UNION B EXCEPT C is Except(Union(A, B), C): left associative,
            // and no operator binds tighter than another (§M3.2).
            void parseQuery(QueryExpr& out)
            {
                DepthGuard guard(*this);
                parseQueryOperand(out);
                while (const std::optional<SetOpKind> found = setOpKindOf(peek().kind))
                {
                    guard.enter(); // the left spine is one set operation deeper
                    const SourcePos pos = advance().pos;
                    SetOpKind kind = *found;
                    if (kind == SetOpKind::Union && accept(SqlTokenKind::All))
                    {
                        kind = SetOpKind::UnionAll;
                    }
                    makeSetOp(kind, pos, out);
                    parseQueryOperand(std::get<SetOperation>(out.node).operands[1]);
                }
            }

            // select_block ::= SELECT ... | "(" query ")"
            // A parenthesised query is returned as a query, so
            // A UNION (B EXCEPT C) keeps the grouping the user wrote.
            void parseQueryOperand(QueryExpr& out)
            {
                if (peek().kind == SqlTokenKind::LeftParen)
                {
                    DepthGuard guard(*this);
                    guard.enter();
                    advance();
                    parseQuery(out);
                    expect(SqlTokenKind::RightParen, "')'");
                    return;
                }
                if (peek().kind != SqlTokenKind::Select)
                {
                    throw syntaxError("SELECT or '('");
                }
                parseSelectBlock(out.node.emplace<SelectBlock>());
            }

            // SELECT [ DISTINCT ] select_list FROM from_list [ WHERE condition ]
            //   [ GROUP BY col_list [ HAVING condition ] ] [ ORDER BY sort_list ]
            //   [ LIMIT int ]
            // The clauses are tried in this fixed order. A clause written out
            // of order is not consumed here and fails at the statement end.
            // HAVING is only looked for right after a GROUP BY list.
            // This method is on the recursion path (FROM subqueries), so the
            // select list and the clauses after FROM are parsed in their own
            // methods, which keeps this frame small.
            void parseSelectBlock(SelectBlock& out)
            {
                out.pos = advance().pos; // SELECT
                out.distinct = accept(SqlTokenKind::Distinct);
                parseSelectList(out);
                expect(SqlTokenKind::From, "FROM");
                parseFrom(out.from);
                parseClausesAfterFrom(out);
            }

            // select_list ::= "*" | select_item { "," select_item }
            void parseSelectList(SelectBlock& out)
            {
                if (peek().kind == SqlTokenKind::Star)
                {
                    out.star = advance().pos;
                    return;
                }
                do
                {
                    parseSelectItem(out.items.emplace_back());
                } while (accept(SqlTokenKind::Comma));
            }

            // [ WHERE ... ] [ GROUP BY ... [ HAVING ... ] ] [ ORDER BY ... ] [ LIMIT n ]
            void parseClausesAfterFrom(SelectBlock& out)
            {
                if (accept(SqlTokenKind::Where))
                {
                    parseOr(out.where.emplace());
                }
                if (accept(SqlTokenKind::Group))
                {
                    expect(SqlTokenKind::By, "BY");
                    do
                    {
                        out.groupBy.push_back(parseColumn());
                    } while (accept(SqlTokenKind::Comma));
                    if (accept(SqlTokenKind::Having))
                    {
                        parseOr(out.having.emplace());
                    }
                }
                if (accept(SqlTokenKind::Order))
                {
                    expect(SqlTokenKind::By, "BY");
                    do
                    {
                        parseSortItem(out.orderBy.emplace_back());
                    } while (accept(SqlTokenKind::Comma));
                }
                if (accept(SqlTokenKind::Limit))
                {
                    // A negative count is refused at the number; 0 is allowed
                    // and returns no rows.
                    if (peek().kind != SqlTokenKind::Integer || peek().intValue < 0)
                    {
                        throw syntaxError("a non-negative integer");
                    }
                    out.limit = advance().intValue;
                }
            }

            // select_item ::= ( column | aggregate ) [ AS ident ]
            // A bare alias without AS (`SELECT name n`) is not in the grammar:
            // the `n` is left over and the caller reports "expected FROM".
            void parseSelectItem(SelectItem& out)
            {
                if (aggregateFunctionOf(peek().kind))
                {
                    out.expression = parseAggregateCall();
                }
                else if (peek().kind == SqlTokenKind::Identifier)
                {
                    out.expression = parseColumn();
                }
                else
                {
                    throw syntaxError("a column, an aggregate or '*'");
                }
                if (accept(SqlTokenKind::As))
                {
                    out.alias = parseName("an alias after AS");
                }
            }

            // from_list ::= table_ref { "," table_ref }
            //             | table_ref JOIN table_ref ON condition { JOIN table_ref ON condition }
            // The token after the first table decides the form. The forms
            // never mix: a JOIN after a comma list (or a comma after a JOIN
            // chain) is left over and fails at the statement end. Keeping the
            // forms apart is what lets the translator emit `cross` for commas
            // and `join` for JOIN ... ON (examples 5.3 and 5.2).
            void parseFrom(FromClause& out)
            {
                parseTableRef(out.tables.emplace_back());
                if (peek().kind == SqlTokenKind::Comma)
                {
                    while (accept(SqlTokenKind::Comma))
                    {
                        parseTableRef(out.tables.emplace_back());
                    }
                    return;
                }
                while (accept(SqlTokenKind::Join))
                {
                    parseTableRef(out.tables.emplace_back());
                    expect(SqlTokenKind::On, "ON");
                    parseOr(out.onConditions.emplace_back());
                }
            }

            // table_ref ::= ident [ [ AS ] ident ] | "(" query ")" AS ident
            // A subquery needs AS and an alias, as in the grammar (§M3.3).
            // This method is on the recursion path (subquery inside
            // subquery), so the base-table case lives in its own method to
            // keep this frame small.
            void parseTableRef(TableRef& out)
            {
                if (peek().kind != SqlTokenKind::LeftParen)
                {
                    parseBaseTable(out);
                    return;
                }
                DepthGuard guard(*this);
                guard.enter();
                advance();
                parseQuery(out.subquery.emplace_back());
                parseSubqueryEnd(out);
            }

            // ")" AS ident, after a FROM subquery.
            void parseSubqueryEnd(TableRef& out)
            {
                expect(SqlTokenKind::RightParen, "')'");
                expect(SqlTokenKind::As, "AS and an alias (a subquery in FROM needs one)");
                out.alias = parseName("an alias after AS");
            }

            // A base table with an optional alias. AS is optional here because
            // the spec's own examples write `Emp e` (5.2, 5.3, 5.5).
            void parseBaseTable(TableRef& out)
            {
                out.table = parseName("a table name");
                if (accept(SqlTokenKind::As))
                {
                    out.alias = parseName("an alias after AS");
                }
                else if (peek().kind == SqlTokenKind::Identifier)
                {
                    // Keywords are reserved, so an identifier here can only be
                    // an alias (`FROM Emp e WHERE ...`).
                    out.alias = parseName("an alias");
                }
            }

            // sort_item ::= column [ ASC | DESC ], ASC by default.
            void parseSortItem(SortItem& out)
            {
                out.column = parseColumn();
                if (accept(SqlTokenKind::Desc))
                {
                    out.ascending = false;
                }
                else
                {
                    accept(SqlTokenKind::Asc);
                }
            }

            std::vector<SqlToken> tokens_;
            std::size_t index_ = 0;
            int depth_ = 0;
        };

    } // namespace

    SqlCondition parseConditionText(std::string_view text)
    {
        SqlParser parser(tokenizeSql(text));
        SqlCondition condition = parser.parseCondition();
        parser.expectEnd();
        return condition;
    }

    std::vector<Statement> parseSqlScript(std::string_view text)
    {
        SqlParser parser(tokenizeSql(text));
        return parser.parseScript();
    }

    QueryExpr parseSqlQuery(std::string_view text)
    {
        SqlParser parser(tokenizeSql(text));
        return parser.parseOneQuery();
    }

} // namespace db::sql
