#include "db/common/evaluator.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "db/common/error.h"

namespace db
{

    BoundCondition bindCondition(const Condition& condition, const Schema& schema)
    {
        return BoundCondition(condition, schema, schema.size(), false);
    }

    BoundCondition bindJoinCondition(const Condition& condition, const Schema& left, const Schema& right)
    {
        return BoundCondition(condition, concatSchemas(left, right), left.size(), true);
    }

    BoundCondition::BoundCondition(const Condition& condition, const Schema& combined, std::size_t leftSize,
                                   bool isJoin)
        : isJoin_(isJoin)
    {
        root_ = bindNode(condition, combined, leftSize);
    }

    // Children are bound before their parent, so a node's children always
    // have smaller indexes and the root is the last node.
    std::size_t BoundCondition::bindNode(const Condition& condition, const Schema& combined, std::size_t leftSize)
    {
        Node node;
        node.kind = condition.kind;
        switch (condition.kind)
        {
            case ConditionKind::Compare:
            {
                Type leftType = Type::Int;
                Type rightType = Type::Int;
                node.op = condition.op;
                node.left = bindOperand(condition.left, combined, leftSize, leftType);
                node.right = bindOperand(condition.right, combined, leftSize, rightType);
                if (leftType != rightType)
                {
                    throw Error(ErrorCategory::Type, "Incompatible comparison operand types: " +
                                                         std::string(typeName(leftType)) + " and " +
                                                         std::string(typeName(rightType)) + ".");
                }
                break;
            }
            case ConditionKind::Not:
                node.first = bindNode(condition.children[0], combined, leftSize);
                break;
            case ConditionKind::And:
            case ConditionKind::Or:
                node.first = bindNode(condition.children[0], combined, leftSize);
                node.second = bindNode(condition.children[1], combined, leftSize);
                break;
        }
        nodes_.push_back(std::move(node));
        return nodes_.size() - 1;
    }

    BoundCondition::BoundOperand BoundCondition::bindOperand(const Operand& operand, const Schema& combined,
                                                             std::size_t leftSize, Type& type) const
    {
        BoundOperand bound;
        if (const Value* value = std::get_if<Value>(&operand))
        {
            bound.source = Source::Literal;
            bound.literal = *value;
            type = value->type();
            return bound;
        }
        const std::size_t index = combined.resolve(std::get<ColumnRef>(operand));
        type = combined.column(index).type;
        if (index < leftSize)
        {
            bound.source = Source::Left;
            bound.index = index;
        }
        else
        {
            bound.source = Source::Right;
            bound.index = index - leftSize;
        }
        return bound;
    }

    bool BoundCondition::evaluate(const Tuple& tuple) const
    {
        if (isJoin_)
        {
            throw std::logic_error("BoundCondition: a join condition needs a left and a right tuple");
        }
        // Every column operand of a non-join condition reads the left tuple.
        return evaluateNode(root_, tuple, tuple);
    }

    bool BoundCondition::evaluate(const Tuple& left, const Tuple& right) const
    {
        if (!isJoin_)
        {
            throw std::logic_error("BoundCondition: a non-join condition takes a single tuple");
        }
        return evaluateNode(root_, left, right);
    }

    bool BoundCondition::evaluateNode(std::size_t index, const Tuple& left, const Tuple& right) const
    {
        const Node& node = nodes_[index];
        switch (node.kind)
        {
            case ConditionKind::And:
                return evaluateNode(node.first, left, right) && evaluateNode(node.second, left, right);
            case ConditionKind::Or:
                return evaluateNode(node.first, left, right) || evaluateNode(node.second, left, right);
            case ConditionKind::Not:
                return !evaluateNode(node.first, left, right);
            case ConditionKind::Compare:
                break;
        }

        // lambda function
        const auto operandValue = [&](const BoundOperand& operand) -> const Value&
        {
            switch (operand.source)
            {
                case Source::Literal:
                    return *operand.literal;
                case Source::Left:
                    return left.at(operand.index);
                case Source::Right:
                    return right.at(operand.index);
            }
            throw std::logic_error("BoundCondition: unknown operand source");
        };
        const int order = compareValues(operandValue(node.left), operandValue(node.right));
        switch (node.op)
        {
            case CompareOp::Equal:
                return order == 0;
            case CompareOp::NotEqual:
                return order != 0;
            case CompareOp::Less:
                return order < 0;
            case CompareOp::LessEqual:
                return order <= 0;
            case CompareOp::Greater:
                return order > 0;
            case CompareOp::GreaterEqual:
                return order >= 0;
        }
        throw std::logic_error("BoundCondition: unknown CompareOp value");
    }

} // namespace db
