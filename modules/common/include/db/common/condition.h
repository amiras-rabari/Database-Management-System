#pragma once

/// @file
/// Conditions (`R.a > 500 and R.b = 'cs'`) as a value-semantic tree: the
/// standalone parser, the printer in two styles, and the helpers the
/// optimizer and the join operators use.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "db/common/schema.h"
#include "db/common/value.h"

namespace db
{

    /// A comparison operator.
    enum class CompareOp
    {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual
    };

    /// The symbol a comparison prints as: `=`, `<>`, `<`, `<=`, `>`, `>=`.
    std::string_view compareOpSymbol(CompareOp op);

    /// What a condition node is.
    enum class ConditionKind
    {
        /// `left op right`.
        Compare,
        /// Both children hold.
        And,
        /// At least one child holds.
        Or,
        /// The single child does not hold.
        Not
    };

    /// One side of a comparison: a column or a literal value.
    using Operand = std::variant<ColumnRef, Value>;

    /// A condition tree node. Copyable, no pointers. Build nodes with the
    /// named constructors; the fields a kind does not use keep their defaults.
    ///
    /// And and Or are binary, so the tree keeps the shape the text was
    /// written in: `a = 1 and b = 2 and c = 3` is And(And(a, b), c).
    struct Condition
    {
        /// What this node is. Enums and primitive types (int, double, raw pointers) do not have
        /// constructors. Without = value, they contain uninitialized memory garbage,
        /// hence both enum here are initialized with default value that can be overridden.
        ConditionKind kind = ConditionKind::Compare;
        /// The operator of a Compare node.
        CompareOp op = CompareOp::Equal;
        /// The left operand of a Compare node.
        Operand left;
        /// The right operand of a Compare node.
        Operand right;
        /// Two children for And and Or, one for Not, none for Compare.
        /// A struct containing a vector of itself is allowed.
        /// The vector stores the children on the heap, so the struct's size is fixed.
        std::vector<Condition> children;

        /// The comparison `left op right`.
        static Condition compare(Operand left, CompareOp op, Operand right);
        /// `left and right`.
        static Condition andOf(Condition left, Condition right);
        /// `left or right`.
        static Condition orOf(Condition left, Condition right);
        /// `not operand`.
        static Condition notOf(Condition operand);
    };

    /// True when both trees have the same shape, operators and operands
    /// (fields that a node's kind does not use are ignored).
    bool operator==(const Condition& left, const Condition& right);

    /// Parses a whole text as one condition.
    ///
    /// Grammar (precedence `not` > `and` > `or`, `and`/`or` left-associative):
    /// ```
    /// or_cond    := and_cond { "or" and_cond }
    /// and_cond   := not_cond { "and" not_cond }
    /// not_cond   := "not" not_cond | primary
    /// primary    := "(" or_cond ")" | comparison
    /// comparison := operand ( "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ) operand
    /// operand    := column_ref | integer | string
    /// column_ref := name [ "." name ]
    /// ```
    /// A name is an identifier or an RA keyword (`union = 3` is a comparison
    /// on a column named `union`), never `and`, `or` or `not`.
    ///
    /// Throws Error(Lexical) from tokenize(); Error(Syntax) at the offending
    /// token if the text is not exactly one condition (including empty text
    /// and leftover tokens), or "Condition nested deeper than 1000 levels."
    /// at the `and`, `or`, `not` or `(` that makes the tree deeper than
    /// kMaxNestingDepth. Depth follows the rule in limits.h: along every
    /// path, each and/or/not node and each open `(` is one level, and a
    /// comparison is none. So 1000 nested `not`, 1000 nested `(` or a chain
    /// of 1000 `and` parse; one more is the error.
    Condition parseCondition(std::string_view text);

    /// How toString() places parentheses.
    enum class ConditionStyle
    {
        /// Only where needed: `not (a = 1 and b = 2) or c > 3`. A right child
        /// of `and`/`or` with the same operator is parenthesised, so parsing
        /// the text gives back the same tree.
        Plan,
        /// Around every comparison and every and/or/not:
        /// `((not ((a = 1) and (b = 2))) or (c > 3))`.
        Explicit
    };

    /// The condition as text: lowercase keywords, `<>` for not-equal, INT in
    /// canonical decimal, strings in quotes with `'` doubled. For trees whose
    /// column names are identifiers or RA keywords (every tree the parser
    /// makes), parseCondition(toString(c, style)) == c in both styles.
    std::string toString(const Condition& condition, ConditionStyle style = ConditionStyle::Plan);

    /// The AND parts of a condition, left to right: `a = 1 and (b = 2 and
    /// c = 3)` gives three parts. A condition that is not an And gives itself.
    std::vector<Condition> splitConjuncts(const Condition& condition);

    /// The parts joined left-nested with `and`: ((p0 and p1) and p2). One part
    /// gives itself. Throws std::logic_error if `parts` is empty.
    Condition conjoin(std::vector<Condition> parts);

    /// Every column the condition refers to, in order of first appearance,
    /// each once. `R.a` and a bare `a` are different refs and both appear.
    std::vector<ColumnRef> columnsUsed(const Condition& condition);

    /// For a join condition that is a conjunction of column = column
    /// equalities, each between one column of `left` and one column of
    /// `right` (either way round), the (left index, right index) pair of each
    /// equality, in conjunct order. std::nullopt when any conjunct is
    /// something else (another operator, a literal, an or/not, two columns of
    /// the same side). Columns resolve against the left columns followed by
    /// the right ones (Schema::resolve rules), and its Name/Ambiguity errors
    /// are thrown, as is concatSchemas' Error(Schema). Types are not checked.
    std::optional<std::vector<std::pair<std::size_t, std::size_t>>>
    equiJoinKeys(const Condition& condition, const Schema& left, const Schema& right);

    /// A copy of the condition with every column ref replaced by
    /// `mapping(ref)`.
    Condition mapColumns(const Condition& condition,
                         const std::function<ColumnRef(const ColumnRef&)>& mapping);

    /// A copy of the condition in which every ref qualified `from` is
    /// qualified `to` instead (`e.name` becomes `Emp.name`). Bare refs and
    /// other qualifiers are left alone.
    Condition replaceQualifier(const Condition& condition, const std::string& from, const std::string& to);

} // namespace db
