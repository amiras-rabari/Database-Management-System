// explainAst: the parse tree as indented text (§M3.7, `sql parse --explain`).
//
// One output string is passed down by reference and every node appends its
// lines to it, so no level builds and copies a string of its own.
//
// Stack size: the printer recurses once per FROM subquery, set operator,
// AND, OR and NOT, and the parser accepts 1000 of those levels
// (kMaxNestingDepth). So the recursive functions (appendQuery,
// appendSelectBlock, appendTableRef, appendCondition) keep only references
// in their frames; the short-lived strings for one line are built in the
// non-recursive helpers.

#include <db/sql/explain_ast.h>

#include <db/common/condition.h>
#include <db/common/plan.h>
#include <db/common/value.h>
#include <db/sql/ast.h>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace db::sql
{

    namespace
    {

        // Starts a new line at this depth: two spaces per level.
        void indent(std::string& out, int depth)
        {
            out.append(static_cast<std::size_t>(2 * depth), ' ');
        }

        // --- One-line pieces (not recursive) ----------------------------

        // `name` or `qualifier.name`, as written.
        void appendColumn(std::string& out, const ColumnName& column)
        {
            if (column.qualifier)
            {
                out += *column.qualifier;
                out += '.';
            }
            out += column.name;
        }

        // An INT as digits; a VARCHAR in quotes with each ' written twice,
        // so the text is valid SQL again (`'O''Brien'`).
        void appendLiteral(std::string& out, const Value& value)
        {
            if (value.type() == Type::Int)
            {
                out += std::to_string(value.asInt());
                return;
            }
            out += '\'';
            for (const char c : value.asVarchar())
            {
                if (c == '\'')
                {
                    out += '\'';
                }
                out += c;
            }
            out += '\'';
        }

        const char* aggregateKeyword(AggregateFunction function)
        {
            switch (function)
            {
            case AggregateFunction::Count:
                return "COUNT";
            case AggregateFunction::Sum:
                return "SUM";
            case AggregateFunction::Avg:
                return "AVG";
            case AggregateFunction::Min:
                return "MIN";
            case AggregateFunction::Max:
                return "MAX";
            }
            return "";
        }

        // `COUNT(*)`, `AVG(salary)`.
        void appendAggregate(std::string& out, const AggregateCall& call)
        {
            out += aggregateKeyword(call.function);
            out += '(';
            if (call.argument)
            {
                appendColumn(out, *call.argument);
            }
            else
            {
                out += '*';
            }
            out += ')';
        }

        void appendOperand(std::string& out, const Operand& operand)
        {
            if (const ColumnName* column = std::get_if<ColumnName>(&operand))
            {
                appendColumn(out, *column);
            }
            else if (const Literal* literal = std::get_if<Literal>(&operand))
            {
                appendLiteral(out, literal->value);
            }
            else
            {
                appendAggregate(out, std::get<AggregateCall>(operand));
            }
        }

        // The operator as SQL writes it (`<>`, not `!=`, which is not SQL).
        const char* compareText(CompareOp op)
        {
            switch (op)
            {
            case CompareOp::Equal:
                return " = ";
            case CompareOp::NotEqual:
                return " <> ";
            case CompareOp::Less:
                return " < ";
            case CompareOp::LessEqual:
                return " <= ";
            case CompareOp::Greater:
                return " > ";
            case CompareOp::GreaterEqual:
                return " >= ";
            }
            return "";
        }

        // --- Conditions (recursive) -------------------------------------

        void appendCondition(std::string& out, const SqlCondition& condition);

        // A child of AND, OR or NOT. A comparison is printed as it is; any
        // other child gets parentheses, so the tree the parser built can be
        // read straight off the text: `a = 1 OR (b = 2 AND c = 3)`.
        void appendChild(std::string& out, const SqlCondition& child)
        {
            if (child.kind == ConditionKind::Compare)
            {
                appendCondition(out, child);
                return;
            }
            out += '(';
            appendCondition(out, child);
            out += ')';
        }

        void appendCondition(std::string& out, const SqlCondition& condition)
        {
            switch (condition.kind)
            {
            case ConditionKind::Compare:
                appendOperand(out, condition.left);
                out += compareText(condition.op);
                appendOperand(out, condition.right);
                return;
            case ConditionKind::Not:
                out += "NOT ";
                appendChild(out, condition.children[0]);
                return;
            case ConditionKind::And:
            case ConditionKind::Or:
                appendChild(out, condition.children[0]);
                out += condition.kind == ConditionKind::And ? " AND " : " OR ";
                appendChild(out, condition.children[1]);
                return;
            }
        }

        // `<label>: <condition>` on its own line (where, having, on).
        void appendConditionLine(std::string& out, int depth, const char* label, const SqlCondition& condition)
        {
            indent(out, depth);
            out += label;
            appendCondition(out, condition);
            out += '\n';
        }

        // --- Select block lines (not recursive) -------------------------

        // `item: COUNT(*) AS n`, or `select: *` for SELECT *.
        void appendSelectList(std::string& out, int depth, const SelectBlock& block)
        {
            if (block.star)
            {
                indent(out, depth);
                out += "select: *\n";
                return;
            }
            for (const SelectItem& item : block.items)
            {
                indent(out, depth);
                out += "item: ";
                if (const ColumnName* column = std::get_if<ColumnName>(&item.expression))
                {
                    appendColumn(out, *column);
                }
                else
                {
                    appendAggregate(out, std::get<AggregateCall>(item.expression));
                }
                if (item.alias)
                {
                    out += " AS ";
                    out += item.alias->text;
                }
                out += '\n';
            }
        }

        // The FROM line names the form, because the translator turns a comma
        // list into `cross` and a JOIN chain into `join` (examples 5.2, 5.3).
        void appendFromLine(std::string& out, int depth, const FromClause& from)
        {
            indent(out, depth);
            if (from.tables.size() == 1)
            {
                out += "from\n";
            }
            else if (from.onConditions.empty())
            {
                out += "from: comma list\n";
            }
            else
            {
                out += "from: join chain\n";
            }
        }

        // `table: Emp AS e`.
        void appendBaseTable(std::string& out, int depth, const TableRef& table)
        {
            indent(out, depth);
            out += "table: ";
            out += table.table.text;
            if (table.alias)
            {
                out += " AS ";
                out += table.alias->text;
            }
            out += '\n';
        }

        // `subquery AS t`. The parser makes sure a subquery has an alias.
        void appendSubqueryLine(std::string& out, int depth, const TableRef& table)
        {
            indent(out, depth);
            out += "subquery AS ";
            out += table.alias->text;
            out += '\n';
        }

        // The clauses after FROM, each on one line, in the order they are
        // written: where, group by, having, order by, limit.
        void appendClausesAfterFrom(std::string& out, int depth, const SelectBlock& block)
        {
            if (block.where)
            {
                appendConditionLine(out, depth, "where: ", *block.where);
            }
            if (!block.groupBy.empty())
            {
                indent(out, depth);
                out += "group by: ";
                for (std::size_t i = 0; i < block.groupBy.size(); ++i)
                {
                    out += i == 0 ? "" : ", ";
                    appendColumn(out, block.groupBy[i]);
                }
                out += '\n';
            }
            if (block.having)
            {
                appendConditionLine(out, depth, "having: ", *block.having);
            }
            if (!block.orderBy.empty())
            {
                indent(out, depth);
                out += "order by: ";
                for (std::size_t i = 0; i < block.orderBy.size(); ++i)
                {
                    out += i == 0 ? "" : ", ";
                    appendColumn(out, block.orderBy[i].column);
                    out += block.orderBy[i].ascending ? " ASC" : " DESC";
                }
                out += '\n';
            }
            if (block.limit)
            {
                indent(out, depth);
                out += "limit: " + std::to_string(*block.limit) + "\n";
            }
        }

        // --- Queries (recursive) ----------------------------------------

        void appendQuery(std::string& out, int depth, const QueryExpr& query);

        // One FROM entry. The ON condition of a JOIN chain is printed one
        // level under the table it joins in: onConditions[k] belongs to
        // tables[k + 1] (see FromClause).
        void appendTableRef(std::string& out, int depth, const TableRef& table)
        {
            if (table.subquery.empty())
            {
                appendBaseTable(out, depth, table);
                return;
            }
            appendSubqueryLine(out, depth, table);
            appendQuery(out, depth + 1, table.subquery[0]);
        }

        void appendSelectBlock(std::string& out, int depth, const SelectBlock& block)
        {
            indent(out, depth);
            out += block.distinct ? "select_block distinct\n" : "select_block\n";
            appendSelectList(out, depth + 1, block);
            appendFromLine(out, depth + 1, block.from);
            for (std::size_t i = 0; i < block.from.tables.size(); ++i)
            {
                appendTableRef(out, depth + 2, block.from.tables[i]);
                if (i > 0 && !block.from.onConditions.empty())
                {
                    appendConditionLine(out, depth + 3, "on: ", block.from.onConditions[i - 1]);
                }
            }
            appendClausesAfterFrom(out, depth + 1, block);
        }

        // The set operator names are the logical plan's names (§M3.5), so
        // UNION and UNION ALL print differently (example 5.7).
        const char* setOpName(SetOpKind kind)
        {
            switch (kind)
            {
            case SetOpKind::Union:
                return "union\n";
            case SetOpKind::UnionAll:
                return "union_all\n";
            case SetOpKind::Intersect:
                return "intersect\n";
            case SetOpKind::Except:
                return "except\n";
            }
            return "";
        }

        void appendQuery(std::string& out, int depth, const QueryExpr& query)
        {
            if (const SelectBlock* block = std::get_if<SelectBlock>(&query.node))
            {
                appendSelectBlock(out, depth, *block);
                return;
            }
            const SetOperation& operation = std::get<SetOperation>(query.node);
            indent(out, depth);
            out += setOpName(operation.kind);
            appendQuery(out, depth + 1, operation.operands[0]);
            appendQuery(out, depth + 1, operation.operands[1]);
        }

        // --- CREATE TABLE and INSERT ------------------------------------

        void appendCreate(std::string& out, const CreateTableStmt& create)
        {
            out += "create_table " + create.table.text + "\n";
            for (const ColumnDef& column : create.columns)
            {
                indent(out, 1);
                out += "column: " + column.name.text;
                if (column.type == Type::Int)
                {
                    out += " INT\n";
                }
                else
                {
                    out += " VARCHAR(" + std::to_string(column.varcharLength) + ")\n";
                }
            }
        }

        void appendInsert(std::string& out, const InsertStmt& insert)
        {
            out += "insert " + insert.table.text + "\n";
            indent(out, 1);
            out += "values: ";
            for (std::size_t i = 0; i < insert.values.size(); ++i)
            {
                out += i == 0 ? "" : ", ";
                appendLiteral(out, insert.values[i].value);
            }
            out += '\n';
        }

    } // namespace

    std::string explainAst(const Statement& statement)
    {
        std::string out;
        if (const CreateTableStmt* create = std::get_if<CreateTableStmt>(&statement))
        {
            appendCreate(out, *create);
        }
        else if (const InsertStmt* insert = std::get_if<InsertStmt>(&statement))
        {
            appendInsert(out, *insert);
        }
        else
        {
            appendQuery(out, 0, std::get<QueryExpr>(statement));
        }
        return out;
    }

} // namespace db::sql
