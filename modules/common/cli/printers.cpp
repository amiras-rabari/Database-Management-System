#include "printers.h"

#include "db/common/condition.h"
#include "db/common/value.h"

#include <cstddef>

namespace ra
{
    namespace
    {
        std::string nodeLabel(const db::PlanNode& node)
        {
            std::string label(db::opName(node.op));
            switch (node.op)
            {
            case db::PlanOp::Scan:
                return label + " " + node.table;
            case db::PlanOp::Select:
            case db::PlanOp::Join:
                return label + " [" + db::toString(*node.condition) + "]";
            case db::PlanOp::Project:
            {
                std::string columns;
                for (const db::ColumnRef& column : node.columns)
                {
                    columns += (columns.empty() ? "" : ", ") + column.toString();
                }
                return label + " [" + columns + "]";
            }
            case db::PlanOp::Rename:
                return label + " [" + node.alias + "]";
            default:
                return label;
            }
        }

        // Children are drawn below their parent, the last one with "|____ ";
        // `prefix` carries the vertical lines of the ancestors.
        void printChildren(std::ostream& out, const db::PlanNode& node, const std::string& prefix)
        {
            for (std::size_t i = 0; i < node.children.size(); ++i)
            {
                const bool isLast = i + 1 == node.children.size();
                out << prefix << (isLast ? "|____ " : "|---- ") << nodeLabel(node.children[i]) << "\n";
                printChildren(out, node.children[i], prefix + (isLast ? "      " : "|     "));
            }
        }
    }

    std::string schemaText(const db::Schema& schema)
    {
        std::string text;
        for (const db::Column& column : schema.columns())
        {
            if (!text.empty())
            {
                text += ", ";
            }
            text += column.qualifiedName() + ":" + std::string(db::typeName(column.type));
            if (column.type == db::Type::Varchar)
            {
                text += "(" + std::to_string(column.varcharLength) + ")";
            }
        }
        return text;
    }

    void printTable(std::ostream& out, const std::string& name, const db::Schema& schema,
                    const std::vector<db::Tuple>& rows)
    {
        out << name << " (";
        for (std::size_t i = 0; i < schema.size(); ++i)
        {
            out << schema.column(i).qualifiedName() << (i + 1 < schema.size() ? ", " : "");
        }
        out << ") =\n";

        for (const db::Tuple& row : rows)
        {
            for (const db::Value& value : row)
            {
                out << value.toText() << " ";
            }
            out << "\n";
        }
        out << "\n";
    }

    void printPlan(std::ostream& out, const db::PlanNode& plan)
    {
        out << nodeLabel(plan) << "\n";
        printChildren(out, plan, "");
    }
}
