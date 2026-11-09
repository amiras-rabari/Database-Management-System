// compileSql: each parsed statement becomes a checked value (chunk 11).
// CREATE TABLE and INSERT are only checked here; the shell executes them
// through storage. Queries go to translateQuery.

#include <db/sql/compile.h>

#include <db/sql/ast.h>
#include <db/sql/parser.h>
#include <db/sql/table_lookup.h>
#include <db/sql/translate.h>

#include "semantic/checks.h"

#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/common/value.h>

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
        // CREATE TABLE: the columns qualified by the table name, as
        // TableLookup returns them. The Schema constructor is the project's
        // one check of column types (VARCHAR n in 1..255) and of duplicate
        // names (DECISIONS 2026-10-26), so nothing is checked twice here.
        // Its error has no position; it is thrown again at the table name.
        // The message already names the column (`Column 'T.b' is ...`).
        CreateTablePlan compileCreate(const CreateTableStmt& statement)
        {
            std::vector<Column> columns;
            for (const ColumnDef& def : statement.columns)
            {
                columns.push_back(Column{statement.table.text, def.name.text, def.type, def.varcharLength});
            }
            try
            {
                return CreateTablePlan{statement.table.text, Schema(std::move(columns))};
            }
            catch (const Error& error)
            {
                failAt(error.category(), error.message(), statement.table.pos);
            }
        }

        // INSERT: the table must exist and every column gets exactly one
        // literal (no NULLs). For each literal the type is checked FIRST:
        // the parser already made `'30'` a VARCHAR value, and common's
        // parseValue("30", INT column) would accept it as 30, silently
        // turning a type error into a valid row. Only then does a string go
        // through parseValue, the project's one VARCHAR(n) length rule.
        // An INT literal is used as it is: the lexer already checked its
        // int32 range.
        InsertPlan compileInsert(const InsertStmt& statement, const TableLookup& lookup)
        {
            const std::optional<Schema> schema = lookup(statement.table.text);
            if (!schema)
            {
                failAt(ErrorCategory::Name, "Unknown table '" + statement.table.text + "'.", statement.table.pos);
            }
            if (statement.values.size() != schema->size())
            {
                failAt(ErrorCategory::Schema,
                       "INSERT has " + std::to_string(statement.values.size()) + " values but table '"
                           + statement.table.text + "' has " + std::to_string(schema->size()) + " columns.",
                       statement.table.pos);
            }

            InsertPlan plan{statement.table.text, {}};
            for (std::size_t i = 0; i < statement.values.size(); ++i)
            {
                const Literal& literal = statement.values[i];
                const Column& column = schema->column(i);
                if (literal.value.type() != column.type)
                {
                    failAt(ErrorCategory::Type,
                           "Value '" + literal.value.toText() + "' is " + std::string(typeName(literal.value.type()))
                               + ", but column '" + column.qualifiedName() + "' is "
                               + std::string(typeName(column.type)) + ".",
                           literal.pos);
                }
                if (column.type == Type::Int)
                {
                    plan.tuple.push_back(literal.value);
                    continue;
                }
                try
                {
                    plan.tuple.push_back(parseValue(literal.value.asVarchar(), column));
                }
                catch (const Error& error)
                {
                    failAt(error.category(), error.message(), literal.pos);
                }
            }
            return plan;
        }
    } // namespace

    std::vector<CompiledStatement> compileSql(std::string_view text, const TableLookup& lookup)
    {
        // The whole script is parsed first, so a syntax error anywhere stops
        // everything before any statement is compiled.
        const std::vector<Statement> statements = parseSqlScript(text);
        std::vector<CompiledStatement> compiled;
        for (const Statement& statement : statements)
        {
            if (const auto* create = std::get_if<CreateTableStmt>(&statement))
            {
                compiled.push_back(compileCreate(*create));
            }
            else if (const auto* insert = std::get_if<InsertStmt>(&statement))
            {
                compiled.push_back(compileInsert(*insert, lookup));
            }
            else
            {
                compiled.push_back(translateQuery(std::get<QueryExpr>(statement), lookup));
            }
        }
        return compiled;
    }
} // namespace db::sql
