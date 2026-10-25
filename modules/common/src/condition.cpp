#include "db/common/condition.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "condition_parser.h"
#include "db/common/tokenizer.h"

namespace db
{

    namespace
    {

        // Higher binds tighter.
        int precedence(ConditionKind kind)
        {
            switch (kind)
            {
                case ConditionKind::Or:
                    return 1;
                case ConditionKind::And:
                    return 2;
                case ConditionKind::Not:
                    return 3;
                case ConditionKind::Compare:
                    return 4;
            }
            throw std::logic_error("precedence: unknown ConditionKind value");
        }

        std::string quoted(const std::string& text)
        {
            std::string out = "'";
            for (char c : text)
            {
                if (c == '\'')
                {
                    out += '\'';
                }
                out += c;
            }
            return out + "'";
        }

        // Operand is nothing but column reference or value container ,
        // its an alias for variant<ColumnRef, Value>
        // so its formate is std::variant<ColumnRef, Value>
        std::string operandText(const Operand& operand)
        {
            // simply this check if we got column ref or we have value if column ref is not present then
            //we simply extract the value as it is guaranteed by variant that either we have column ref or value
            //look at header file if column ref it returns true here cause we using get_if
            if (const ColumnRef* ref = std::get_if<ColumnRef>(&operand))
            {
                return ref->toString();
            }
            const Value& value = std::get<Value>(operand);
            return value.type() == Type::Int ? value.toText() : quoted(value.asVarchar());
        }

        std::string comparisonText(const Condition& condition)
        {
            return operandText(condition.left) + " " + std::string(compareOpSymbol(condition.op)) + " " +
                   operandText(condition.right);
        }

        std::string_view connective(ConditionKind kind)
        {
            return kind == ConditionKind::And ? " and " : " or ";
        }

        //puts parenthesis around node these are just print helpers
        std::string explicitText(const Condition& condition)
        {
            switch (condition.kind)
            {
                case ConditionKind::Compare:
                    return "(" + comparisonText(condition) + ")";
                case ConditionKind::Not:
                    return "(not " + explicitText(condition.children[0]) + ")";
                case ConditionKind::And:
                case ConditionKind::Or:
                    return "(" + explicitText(condition.children[0]) + std::string(connective(condition.kind)) +
                           explicitText(condition.children[1]) + ")";
            }
            throw std::logic_error("explicitText: unknown ConditionKind value");
        }

        //planText is the main one. It adds parentheses only where they’re needed
        //so that re-parsing gives the same tree:
        // Left child of a lower precedence gets parentheses: Or(a, b)
        // inside an And prints as (a or b) and c.
        // Right child with lower or equal precedence gets parentheses (<=, line 114).
        // The parser always nests to the left,
        // so And(a, And(b, c)) must print as a and (b and c). Without the parentheses
        // it would re-parse as And(And(a, b), c), which is a different shape.
        // not gets parentheses unless its child is a single comparison: not a = 1,
        // but not (a = 1 and b = 2).
        std::string planText(const Condition& condition)
        {
            switch (condition.kind)
            {
                case ConditionKind::Compare:
                    return comparisonText(condition);
                case ConditionKind::Not:
                {
                    const Condition& operand = condition.children[0];
                    if (operand.kind == ConditionKind::Compare)
                    {
                        return "not " + planText(operand);
                    }
                    return "not (" + planText(operand) + ")";
                }
                case ConditionKind::And:
                case ConditionKind::Or:
                {
                    const int own = precedence(condition.kind);
                    const Condition& left = condition.children[0];
                    const Condition& right = condition.children[1];
                    std::string leftText = planText(left);
                    std::string rightText = planText(right);
                    if (precedence(left.kind) < own)
                    {
                        leftText = "(" + leftText + ")";
                    }
                    // The parser nests same-operator chains to the left, so a
                    // right child of equal precedence needs parentheses to
                    // keep its shape.
                    if (precedence(right.kind) <= own)
                    {
                        rightText = "(" + rightText + ")";
                    }
                    return leftText + std::string(connective(condition.kind)) + rightText;
                }
            }
            throw std::logic_error("planText: unknown ConditionKind value");
        }

        void collectConjuncts(const Condition& condition, std::vector<Condition>& parts)
        {
            if (condition.kind == ConditionKind::And)
            {
                collectConjuncts(condition.children[0], parts);
                collectConjuncts(condition.children[1], parts);
            }
            else
            {
                parts.push_back(condition);
            }
        }

        // Adds a ColumnRef to a list, but only if it hasn't been added yet
        void addColumn(const Operand& operand, std::vector<ColumnRef>& columns)
        {
            const ColumnRef* ref = std::get_if<ColumnRef>(&operand);
            //Iterates through the vector from begin() up to end(), comparing each element
            //against *ref using the == operator. If found: Returns an iterator pointing
            //to the existing match. Comparing the result of std::find against columns.end()
            //evaluates to true only if the column is absent from the vector
            if (ref != nullptr && std::find(columns.begin(), columns.end(), *ref) == columns.end())
            {
                columns.push_back(*ref);
            }
        }

        void collectColumns(const Condition& condition, std::vector<ColumnRef>& columns)
        {
            if (condition.kind != ConditionKind::Compare)
            {
                for (const Condition& child : condition.children)
                {
                    collectColumns(child, columns);
                }
                return;
            }
            addColumn(condition.left, columns);
            addColumn(condition.right, columns);
        }

    // This function answers one simple question: "Is this operand a column name?"
    //
    // If YES: Apply the mapping rule to transform the column name, and return the new version.
    //What mapping(*ref) executes: return ColumnRef("users." + col.name); this is a mapping rule
    //we are using a mapping rule to transform column names and we use std::function to apply it
    // std::function< ... >	A generic wrapper holding a function, method, or lambda.
    // so its formate is std::function<output (input) > mapping rule
    // If NO (it's a raw number or text like 42 or 'hello'): Leave it alone and return it unchanged.
        Operand mapOperand(const Operand& operand, const std::function<ColumnRef(const ColumnRef&)>& mapping)
        {
            if (const ColumnRef* ref = std::get_if<ColumnRef>(&operand))
            {
                return mapping(*ref);
            }
            return operand;
        }

    } // namespace

    std::string_view compareOpSymbol(CompareOp op)
    {
        switch (op)
        {
            case CompareOp::Equal:
                return "=";
            case CompareOp::NotEqual:
                return "<>";
            case CompareOp::Less:
                return "<";
            case CompareOp::LessEqual:
                return "<=";
            case CompareOp::Greater:
                return ">";
            case CompareOp::GreaterEqual:
                return ">=";
        }
        throw std::logic_error("compareOpSymbol: unknown CompareOp value");
    }

    Condition Condition::compare(Operand left, CompareOp op, Operand right)
    {
        Condition condition;
        condition.kind = ConditionKind::Compare;
        condition.op = op;
        condition.left = std::move(left);
        condition.right = std::move(right);
        return condition;
    }

    Condition Condition::andOf(Condition left, Condition right)
    {
        Condition condition;
        condition.kind = ConditionKind::And;
        condition.children.push_back(std::move(left));
        condition.children.push_back(std::move(right));
        return condition;
    }

    Condition Condition::orOf(Condition left, Condition right)
    {
        Condition condition;
        condition.kind = ConditionKind::Or;
        condition.children.push_back(std::move(left));
        condition.children.push_back(std::move(right));
        return condition;
    }

    Condition Condition::notOf(Condition operand)
    {
        Condition condition;
        condition.kind = ConditionKind::Not;
        condition.children.push_back(std::move(operand));
        return condition;
    }

    // This is C++'s way of overriding public boolean equals(Object o)
    // Overloading operator== defines value equality for custom types,
    // enabling both direct c1 == c2 checks and seamless integration
    // with C++ Standard Library algorithms like std::find
    // compares the kind first. For a Compare node it compares op, left and right;
    // otherwise it compares children. vector == vector compares element by element,
    // which calls operator== on each child again,
    // so the whole tree is compared recursively. The unused fields are ignored on purpose.
    bool operator==(const Condition& left, const Condition& right)
    {
        if (left.kind != right.kind)
        {
            return false;
        }
        if (left.kind == ConditionKind::Compare)
        {
            return left.op == right.op && left.left == right.left && left.right == right.right;
        }
        return left.children == right.children;
    }

    Condition parseCondition(std::string_view text)
    {
        const std::vector<Token> tokens = tokenize(text);
        std::size_t position = 0;
        // tokens → tree
        detail::ParsedCondition parsed = detail::parseConditionAt(tokens, position);
        if (tokens[position].kind != TokenKind::End)
        {
//The final check rejects leftovers: a = 1 b parses a = 1, then finds b where the end should be,
//which is a syntax error.
            detail::throwSyntaxError(tokens[position], "Expected end of condition");
        }
        return std::move(parsed.condition);
    }

    // different to string fo different kind  style
    std::string toString(const Condition& condition, ConditionStyle style)
    {
        return style == ConditionStyle::Explicit ? explicitText(condition) : planText(condition);
    }

    std::vector<Condition> splitConjuncts(const Condition& condition)
    {
        std::vector<Condition> parts;
        collectConjuncts(condition, parts);
        return parts;
    }

    Condition conjoin(std::vector<Condition> parts)
    {
        if (parts.empty())
        {
            throw std::logic_error("conjoin: no conditions to join");
        }
        Condition result = std::move(parts[0]);
        for (std::size_t i = 1; i < parts.size(); ++i)
        {
            result = Condition::andOf(std::move(result), std::move(parts[i]));
        }
        return result;
    }

    std::vector<ColumnRef> columnsUsed(const Condition& condition)
    {
        std::vector<ColumnRef> columns;
        collectColumns(condition, columns);
        return columns;
    }

    //// Expression AST: "users.id = orders.user_id"
    // left schema indices:  [0: users.id, 1: users.name]
    // right schema indices: [0: orders.id, 1: orders.user_id]
    //// Output: { {0, 1} } -> left col 0 matches right col 1
    std::optional<std::vector<std::pair<std::size_t, std::size_t>>>
    equiJoinKeys(const Condition& condition, const Schema& left, const Schema& right)
    {
        const Schema combined = concatSchemas(left, right);
        std::vector<std::pair<std::size_t, std::size_t>> keys;
        //to flatten top-level AND expressions into an array of individual condition clauses.
        //then iterate over each individual condition clause.
        for (const Condition& part : splitConjuncts(condition))
        {
            if (part.kind != ConditionKind::Compare || part.op != CompareOp::Equal)
            {
                return std::nullopt;
            }
            const ColumnRef* first = std::get_if<ColumnRef>(&part.left);
            const ColumnRef* second = std::get_if<ColumnRef>(&part.right);
            if (first == nullptr || second == nullptr)
            {
                return std::nullopt;
            }
            // look at schema.h resolve method
            const std::size_t firstIndex = combined.resolve(*first);
            const std::size_t secondIndex = combined.resolve(*second);

            // check if the columns are from the same table , if they are return  std::nullopt
            // cause in combined vector we have 2 table's column in same list,
            // so index should be from different parts of vector
            const bool firstIsLeft = firstIndex < left.size();
            if (firstIsLeft == (secondIndex < left.size()))
            {
                return std::nullopt;
            }
            keys.push_back(firstIsLeft ? std::make_pair(firstIndex, secondIndex - left.size())
                                       : std::make_pair(secondIndex, firstIndex - left.size()));
        }
        return keys;
    }

    Condition mapColumns(const Condition& condition, const std::function<ColumnRef(const ColumnRef&)>& mapping)
    {
        if (condition.kind == ConditionKind::Compare)
        {
            return Condition::compare(mapOperand(condition.left, mapping), condition.op,
                                      mapOperand(condition.right, mapping));
        }
        Condition result;
        result.kind = condition.kind;
        for (const Condition& child : condition.children)
        {
            result.children.push_back(mapColumns(child, mapping));
        }
        return result;
    }

    // const std::string& is pass-by-const-reference.passes an alias (reference) to the existing
    // string, preventing allocation and copying overhead without permitting modification
    Condition replaceQualifier(const Condition& condition, const std::string& from, const std::string& to)
    {

        // The lambda body below IS the 'mapping' function! used throughout to map column name
        // Lambda Capture Clause ([&]): Tells the compiler to capture from and to by reference.
        // [&] means “use the surrounding variables (from, to) by reference”.
        //  -> ColumnRef states the return type
        return mapColumns(condition, [&](const ColumnRef& ref) -> ColumnRef
        {
            if (ref.qualifier && *ref.qualifier == from)
            {
                return ColumnRef{to, ref.name};
            }
            return ref;
        });
    }

} // namespace db
