#include "db/common/schema.h"

#include <set>
#include <string>
#include <utility>

#include "db/common/error.h"

namespace db
{

    std::string ColumnRef::toString() const
    {
        return qualifier ? *qualifier + "." + name : name;
    }

    std::string Column::qualifiedName() const
    {
        return qualifier.empty() ? name : qualifier + "." + name;
    }

    Schema::Schema(std::vector<Column> columns)
        : columns_(std::move(columns))
    {
        std::set<std::string> seen;
        for (const Column& column : columns_)
        {
            // The type rules (G1): VARCHAR(n) needs 1 <= n <= 255 (PostgreSQL
            // rejects VARCHAR(0) too), and an INT has no length.
            if (column.type == Type::Varchar
                && (column.varcharLength < 1 || column.varcharLength > kMaxVarcharLength))
            {
                throw Error(ErrorCategory::Schema, "Column '" + column.qualifiedName() + "' is VARCHAR("
                                                       + std::to_string(column.varcharLength) + "); n must be 1 to "
                                                       + std::to_string(kMaxVarcharLength) + ".");
            }
            if (column.type == Type::Int && column.varcharLength != 0)
            {
                throw Error(ErrorCategory::Schema, "Column '" + column.qualifiedName()
                                                       + "' is INT but has a VARCHAR length of "
                                                       + std::to_string(column.varcharLength) + ".");
            }
            /// seen.insert(x) returns a pair: .first points to the element, and
            /// .second is true if it was newly inserted and false if it was already there.
            /// So !...second means "seen before", which is a duplicate.
            /// What it does: Emp.DID and Dept.DID are fine because their qualified names differ.
            /// Two Emp.DIDs are an error.
            if (!seen.insert(column.qualifiedName()).second)
            {
                throw Error(ErrorCategory::Schema, "Duplicate column '" + column.qualifiedName() + "'.");
            }
        }
    }

    const std::vector<Column>& Schema::columns() const
    {
        return columns_;
    }

    std::size_t Schema::size() const
    {
        return columns_.size();
    }

    //Note:columns_.at(index) is bounds-checked: an index that's too large throws
    //std::out_of_range. columns_[index] would not check, just like C arrays.
    //The public function uses at() for safety. Inside resolve the loop already knows i is valid,
    //so it uses the faster [].
    const Column& Schema::column(std::size_t index) const
    {
        return columns_.at(index);
    }

    std::size_t Schema::resolve(const ColumnRef& ref) const
    {
        if (ref.qualifier)
        {
            for (std::size_t i = 0; i < columns_.size(); ++i)
            {
                if (columns_[i].qualifier == *ref.qualifier && columns_[i].name == ref.name)
                {
                    return i;
                }
            }
            throw Error(ErrorCategory::Name, "Unknown qualified attribute '" + ref.toString() + "'.");
        }

        std::vector<std::size_t> matches;
        for (std::size_t i = 0; i < columns_.size(); ++i)
        {
            if (columns_[i].name == ref.name)
            {
                matches.push_back(i);
            }
        }
        if (matches.empty())
        {
            throw Error(ErrorCategory::Name, "Unknown attribute '" + ref.name + "'.");
        }
        if (matches.size() > 1)
        {
            std::string candidates;
            for (std::size_t index : matches)
            {
                if (!candidates.empty())
                {
                    candidates += ", ";
                }
                candidates += columns_[index].qualifiedName();
            }
            throw Error(ErrorCategory::Ambiguity,
                        "Ambiguous unqualified attribute '" + ref.name + "' (matches " + candidates +
                            "). Use a relation qualifier.");
        }
        return matches.front();
    }

    Value parseValue(std::string_view text, const Column& column)
    {
        if (column.type == Type::Int)
        {
            const std::optional<std::int32_t> value = parseInt32(text);
            if (!value)
            {
                throw Error(ErrorCategory::Type, "'" + std::string(text) + "' is not a valid INT for column " +
                                                     column.qualifiedName() + ".");
            }
            return Value::ofInt(*value);
        }

        if (text.size() > static_cast<std::size_t>(column.varcharLength))
        {
            throw Error(ErrorCategory::Schema, "'" + std::string(text) + "' is longer than VARCHAR(" +
                                                   std::to_string(column.varcharLength) + ") for column " +
                                                   column.qualifiedName() + ".");
        }
        return Value::ofVarchar(std::string(text));
    }


    //for (joins/cross)  What it does: the output schema of Emp × Dept is the Emp columns followed by
    //the Dept columns. Building the result through the Schema constructor re-runs the duplicate check
    //for free. So Emp × Emp, a self-join without renaming, fails because Emp.EID appears twice.
    //The fix is to rename one side first, and that's what requalify is for.

    // insert(where, from, to) appends a range of elements. begin() / end() are iterators,
    // which work like a start pointer
    Schema concatSchemas(const Schema& left, const Schema& right)
    {
        std::vector<Column> columns = left.columns();
        columns.insert(columns.end(), right.columns().begin(), right.columns().end());
        return Schema(std::move(columns));
    }

    Schema requalify(const Schema& schema, const std::string& alias)
    {
        std::vector<Column> columns = schema.columns();
        for (Column& column : columns)
        {
            column.qualifier = alias;
        }
        return Schema(std::move(columns));
    }

    //// write the positions into the caller's vector // indices_ = [4, 0]
    //// input row [1,'Ann',10,10,'Sales']  →  ['Sales', 1]
    ///so in one pass we resolve the indexes of the columns we want to project
    Schema projectSchema(const Schema& input, const std::vector<ColumnRef>& refs, std::vector<std::size_t>* indices)
    {
        std::vector<std::size_t> picked;
        std::vector<Column> columns;
        for (const ColumnRef& ref : refs)
        {
            const std::size_t index = input.resolve(ref);
            for (std::size_t earlier : picked)
            {
                if (earlier == index)
                {
                    throw Error(ErrorCategory::Schema,
                                "Duplicate projection attribute '" + input.column(index).qualifiedName() + "'.");
                }
            }
            picked.push_back(index);
            columns.push_back(input.column(index));
        }
        if (indices != nullptr)
        {
            *indices = std::move(picked);
        }
        return Schema(std::move(columns));
    }

    void checkUnionCompatible(const Schema& left, const Schema& right, std::string_view opName)
    {
        const std::string prefix = "Relation schemas are not union-compatible for '" + std::string(opName) + "': ";
        if (left.size() != right.size())
        {
            throw Error(ErrorCategory::Schema, prefix + "column counts differ (" + std::to_string(left.size()) +
                                                   " vs " + std::to_string(right.size()) + ").");
        }
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            const Type leftType = left.column(i).type;
            const Type rightType = right.column(i).type;
            if (leftType != rightType)
            {
                throw Error(ErrorCategory::Schema, prefix + "column " + std::to_string(i + 1) + " is " +
                                                       std::string(typeName(leftType)) + " on the left but " +
                                                       std::string(typeName(rightType)) + " on the right.");
            }
        }
    }

} // namespace db
