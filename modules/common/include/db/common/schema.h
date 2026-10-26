#pragma once

/// @file
/// Qualified columns, schemas, tuples, and the schema rules shared by plan
/// schema computation and the operators (join/cross, rename, project, set
/// operations).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db/common/value.h"

namespace db
{
    /// Why two types for a column: ColumnRef is what the user typed and may be incomplete (DID).
    /// Column is what exists, always complete (Emp.DID INT). resolve connects the two.
    /// A column as written in a query or condition: `Emp.DID` (qualified) or
    /// `DID` (bare). Identifiers are case-sensitive.
    struct ColumnRef
    {
        /// The relation qualifier, absent for a bare name.
        std::optional<std::string> qualifier;
        /// The column name.
        std::string name;

        /// `qualifier.name`, or `name` when there is no qualifier.
        std::string toString() const;

        /// True when qualifier (or its absence) and name are equal; `Emp.DID`
        /// and a bare `DID` are different refs.
        friend bool operator==(const ColumnRef& left, const ColumnRef& right) = default;
    };

    /// Largest n allowed in VARCHAR(n) (G1: "VARCHAR(n) with n <= 255").
    inline constexpr int kMaxVarcharLength = 255;

    /// One column of a schema. Every column is stored qualified.
    struct Column
    {
        /// The relation (or alias) the column belongs to, e.g. `Emp`.
        /// "Emp"   (always filled, not optional)
        std::string qualifier;
        /// The column name, e.g. `DID`.
        std::string name;
        /// The column type.
        Type type = Type::Int;
        /// Maximum length in bytes n of VARCHAR(n), 1..255; 0 for INT.
        int varcharLength = 0;

        /// `qualifier.name`, or `name` when the qualifier is empty.
        std::string qualifiedName() const;
    };

    /// Ordered list of columns. No two columns share a qualified name.
    class Schema
    {
    public:
        /// An empty schema.
        Schema() = default;
        /// A schema with these columns in this order. Throws Error(Schema)
        /// - "Duplicate column 'Emp.DID'." if two columns share a qualified name;
        /// - "Column 'R.b' is VARCHAR(300); n must be 1 to 255." if a VARCHAR
        ///   length is outside 1..255;
        /// - "Column 'R.a' is INT but has a VARCHAR length of 5." if an INT
        ///   column's varcharLength is not 0.
        /// So every Schema in the project has legal column types.
        explicit Schema(std::vector<Column> columns);

        /// All columns, in order.
        const std::vector<Column>& columns() const;
        /// Number of columns.
        std::size_t size() const;
        /// The column at `index` (0-based). Throws std::out_of_range if
        /// `index >= size()`.
        const Column& column(std::size_t index) const;

        /// Index of the column `ref` names. A qualified ref must match a
        /// column's qualifier and name exactly, else Error(Name). A bare ref
        /// must match the name of exactly one column: none is Error(Name),
        /// more than one is Error(Ambiguity) listing the candidates.
        std::size_t resolve(const ColumnRef& ref) const;

    private:
        std::vector<Column> columns_;
    };

    /// One row: a value per schema column, in schema order.
    using Tuple = std::vector<Value>;

    /// Turns text into a value of `column`'s type: INT through parseInt32,
    /// else Error(Type) "'abc' is not a valid INT for column R.a.";
    /// VARCHAR(n) as the raw bytes, Error(Schema) if longer than n bytes.
    /// The single rule for reading data values in the project.
    Value parseValue(std::string_view text, const Column& column);

    /// Schema of a join or cross product: the left columns, then the right
    /// ones. Throws Error(Schema) if a qualified name occurs on both sides
    /// (e.g. `Emp times Emp` without a rename).
    Schema concatSchemas(const Schema& left, const Schema& right);

    /// The same columns with every qualifier replaced by `alias`
    /// (`Emp.Name` becomes `e.Name`). Throws Error(Schema) if two columns
    /// then share a qualified name.
    Schema requalify(const Schema& schema, const std::string& alias);

    /// Schema of a projection: the columns `refs` resolve to, in `refs` order
    /// (resolution errors as Schema::resolve). Throws Error(Schema)
    /// "Duplicate projection attribute 'Emp.Name'." if two refs name the same
    /// column. When `indices` is not null it receives the input index of each
    /// output column.
    Schema projectSchema(const Schema& input, const std::vector<ColumnRef>& refs,
                         std::vector<std::size_t>* indices = nullptr);

    /// Checks that a set operation (`opName`, e.g. `union`) may combine the
    /// two schemas: same number of columns and the same type at each
    /// position, else Error(Schema). Names and VARCHAR lengths may differ;
    /// the result of a set operation has the LEFT schema.
    void checkUnionCompatible(const Schema& left, const Schema& right, std::string_view opName);

} // namespace db
