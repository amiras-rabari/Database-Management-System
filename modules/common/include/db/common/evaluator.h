#pragma once

/// @file
/// Evaluates a condition against tuples. Binding resolves every column and
/// checks every type once; evaluation then only reads tuple fields.

#include <cstddef>
#include <optional>
#include <vector>

#include "db/common/condition.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace db
{

    // a forward declaration. The two functions below return a BoundCondition, but the class body comes later
    class BoundCondition;

    /// Binds `condition` to tuples of `schema`. Every column resolves by the
    /// Schema::resolve rules (Error(Name) / Error(Ambiguity)); the two
    /// operands of every comparison must have the same type, else
    /// Error(Type) "Incompatible comparison operand types: INT and VARCHAR.".
    /// A comparison of two literals is allowed when their types match.

    BoundCondition bindCondition(const Condition& condition, const Schema& schema);

    /// Binds a join condition to pairs of tuples, one of `left` and one of
    /// `right`. Columns resolve against the left columns followed by the right
    /// ones, so a bare name found on both sides is Error(Ambiguity). Throws
    /// what bindCondition throws, and concatSchemas' Error(Schema).
    /// So a bare DID that exists on both sides is an Ambiguity error.
    BoundCondition bindJoinCondition(const Condition& condition, const Schema& left, const Schema& right);

    /// A condition whose columns are already tuple positions. Evaluation
    /// counts nothing (operators do the counting) and, for tuples that match
    /// the bound schemas, never throws Error. A tuple shorter than its schema
    /// is an internal bug and throws std::out_of_range.
    /// undCondition is flat: every node sits in one vector<Node>,
    /// and the children are indexes (first, second) into that vector.
    /// Picture an array of structs where children point at each other by array index,
    // a = 1 and b = 2   →   nodes_[0] = Compare(a=1)
    //                       nodes_[1] = Compare(b=2)
    //                       nodes_[2] = And(first=0, second=1)   ← root_ = 2
    class BoundCondition
    {
    public:
        /// Whether the condition holds for `tuple`, a tuple of the schema
        /// given to bindCondition. Throws std::logic_error if the condition was
        /// bound with bindJoinCondition.
        bool evaluate(const Tuple& tuple) const;

        /// Whether the condition holds for the pair (`left`, `right`), tuples
        /// of the two schemas given to bindJoinCondition; the tuples are not
        /// concatenated. Throws std::logic_error if the condition was bound
        /// with bindCondition.
        bool evaluate(const Tuple& left, const Tuple& right) const;

    private:
        // Where a comparison operand comes from.
        enum class Source
        {
            Literal,
            Left,
            Right
        };

        struct BoundOperand
        {
            Source source = Source::Literal;
            std::size_t index = 0;
            std::optional<Value> literal;
        };

        // A condition node; `first`/`second` are indexes into nodes_.
        struct Node
        {
            ConditionKind kind = ConditionKind::Compare;
            CompareOp op = CompareOp::Equal;
            BoundOperand left;
            BoundOperand right;
            std::size_t first = 0;
            std::size_t second = 0;
        };

        friend BoundCondition bindCondition(const Condition& condition, const Schema& schema);
        friend BoundCondition bindJoinCondition(const Condition& condition, const Schema& left,
                                                const Schema& right);

        // Binds `condition` against `combined` (one schema, or left + right);
        // columns at or past `leftSize` belong to the right tuple.
        BoundCondition(const Condition& condition, const Schema& combined, std::size_t leftSize,
                       bool isJoin);

        std::size_t bindNode(const Condition& condition, const Schema& combined, std::size_t leftSize);

        BoundOperand bindOperand(const Operand& operand, const Schema& combined, std::size_t leftSize,
                                 Type& type) const;
        bool evaluateNode(std::size_t index, const Tuple& left, const Tuple& right) const;

        std::vector<Node> nodes_;
        std::size_t root_ = 0;
        bool isJoin_ = false;
    };

} // namespace db
