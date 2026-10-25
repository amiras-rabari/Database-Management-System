#include "relation_loader.h"

#include "db/common/error.h"
#include "db/common/value.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>

namespace ra
{
    namespace
    {
        constexpr std::size_t kMaxVarcharLength = 255;

        db::Type typeOf(const TextValue& value)
        {
            return value.kind == TextValueKind::Integer ? db::Type::Int : db::Type::Varchar;
        }

        void checkNames(const TextRelation& relation, const std::vector<Relation>& loaded)
        {
            for (const Relation& earlier : loaded)
            {
                if (earlier.name == relation.name)
                {
                    throw db::Error(db::ErrorCategory::Name,
                                    "Duplicate relation definition for '" + relation.name + "'.",
                                    relation.line, relation.column);
                }
            }
            std::set<std::string> seen;
            for (const std::string& attribute : relation.attributes)
            {
                if (!seen.insert(attribute).second)
                {
                    throw db::Error(db::ErrorCategory::Schema,
                                    "Duplicate attribute '" + attribute + "' in relation '" + relation.name + "'.",
                                    relation.line, relation.column);
                }
            }
        }

        // Checks one value against its column as typed so far and widens a
        // VARCHAR column to fit it.
        void fitValue(const TextRelation& relation, std::size_t rowIndex, std::size_t columnIndex,
                      db::Column& column)
        {
            const TextValue& value = relation.rows[rowIndex].values[columnIndex];
            if (value.kind == TextValueKind::Integer && !db::parseInt32(value.text))
            {
                throw db::Error(db::ErrorCategory::Lexical, "Integer literal out of INT range: " + value.text + ".",
                                value.line, value.column);
            }
            if (typeOf(value) != column.type)
            {
                throw db::Error(db::ErrorCategory::Type,
                                "Type mismatch in relation '" + relation.name + "', row " +
                                    std::to_string(rowIndex + 1) + ", attribute '" + column.name + "'. Expected " +
                                    std::string(db::typeName(column.type)) + " but got " +
                                    std::string(db::typeName(typeOf(value))) + ".",
                                value.line, value.column);
            }
            if (column.type == db::Type::Varchar)
            {
                if (value.text.size() > kMaxVarcharLength)
                {
                    throw db::Error(db::ErrorCategory::Schema,
                                    "Value of " + std::to_string(value.text.size()) + " bytes for " +
                                        column.qualifiedName() + " is longer than the VARCHAR limit of 255 bytes.",
                                    value.line, value.column);
                }
                column.varcharLength = std::max(column.varcharLength, static_cast<int>(value.text.size()));
            }
        }

        std::vector<db::Column> columnsOf(const TextRelation& relation)
        {
            std::vector<db::Column> columns;
            for (std::size_t i = 0; i < relation.attributes.size(); ++i)
            {
                db::Column column;
                column.qualifier = relation.name;
                column.name = relation.attributes[i];
                if (relation.rows.empty())
                {
                    column.type = db::Type::Varchar;
                    column.varcharLength = static_cast<int>(kMaxVarcharLength);
                }
                else
                {
                    column.type = typeOf(relation.rows.front().values[i]);
                    column.varcharLength = column.type == db::Type::Varchar ? 1 : 0;
                }
                columns.push_back(std::move(column));
            }
            for (std::size_t row = 0; row < relation.rows.size(); ++row)
            {
                for (std::size_t i = 0; i < columns.size(); ++i)
                {
                    fitValue(relation, row, i, columns[i]);
                }
            }
            return columns;
        }

        Relation loadRelation(const TextRelation& relation, const std::vector<Relation>& loaded)
        {
            checkNames(relation, loaded);
            Relation result;
            result.name = relation.name;
            result.schema = db::Schema(columnsOf(relation));
            for (const TextRow& row : relation.rows)
            {
                db::Tuple tuple;
                for (std::size_t i = 0; i < row.values.size(); ++i)
                {
                    tuple.push_back(db::parseValue(row.values[i].text, result.schema.column(i)));
                }
                result.rows.push_back(std::move(tuple));
            }
            return result;
        }
    }

    std::vector<Relation> loadRelations(const std::vector<TextRelation>& relations)
    {
        std::vector<Relation> loaded;
        for (const TextRelation& relation : relations)
        {
            loaded.push_back(loadRelation(relation, loaded));
        }
        return loaded;
    }
}
