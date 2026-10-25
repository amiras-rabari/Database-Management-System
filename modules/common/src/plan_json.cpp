#include "db/common/plan_json.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "condition_parser.h"
#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/tokenizer.h"

namespace db
{

    namespace
    {

        // The plan JSON field names. Shown in spec examples: op, table,
        // condition, columns, child, left/right of join, to. The fields of
        // cross, the set operations and distinct follow the spec's pattern
        // (two inputs use left/right, one input uses child).
        constexpr const char* kOp = "op";
        constexpr const char* kTable = "table";
        constexpr const char* kCondition = "condition";
        constexpr const char* kColumns = "columns";
        constexpr const char* kChild = "child";
        constexpr const char* kLeft = "left";
        constexpr const char* kRight = "right";
        constexpr const char* kTo = "to";

        constexpr PlanOp kAllOps[] = {PlanOp::Scan,   PlanOp::Select, PlanOp::Project,   PlanOp::Join,
                                      PlanOp::Cross,  PlanOp::Rename, PlanOp::Union,     PlanOp::Intersect,
                                      PlanOp::Except, PlanOp::Distinct};

        // The fields of each op besides `op`, in the order they are written.
        std::vector<const char*> fieldsOf(PlanOp op)
        {
            switch (op)
            {
                case PlanOp::Scan:
                    return {kTable};
                case PlanOp::Select:
                    return {kCondition, kChild};
                case PlanOp::Project:
                    return {kColumns, kChild};
                case PlanOp::Join:
                    return {kCondition, kLeft, kRight};
                case PlanOp::Rename:
                    return {kTo, kChild};
                case PlanOp::Distinct:
                    return {kChild};
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                    return {kLeft, kRight};
            }
            throw std::logic_error("fieldsOf: unknown PlanOp value");
        }

        std::optional<PlanOp> opFromName(const std::string& name)
        {
            for (PlanOp op : kAllOps)
            {
                if (opName(op) == name)
                {
                    return op;
                }
            }
            return std::nullopt;
        }

        std::size_t inputCount(PlanOp op)
        {
            switch (op)
            {
                case PlanOp::Scan:
                    return 0;
                case PlanOp::Select:
                case PlanOp::Project:
                case PlanOp::Rename:
                case PlanOp::Distinct:
                    return 1;
                case PlanOp::Join:
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                    return 2;
            }
            throw std::logic_error("inputCount: unknown PlanOp value");
        }

        // --- Writing -------------------------------------------------------

        // Fills `out` in place, so a recursion level holds only references.
        void writeNode(const PlanNode& node, PlanJson& out)
        {
            if (node.children.size() != inputCount(node.op))
            {
                throw std::logic_error("toJson: plan node '" + std::string(opName(node.op)) +
                                       "' has the wrong number of inputs");
            }
            if ((node.op == PlanOp::Select || node.op == PlanOp::Join) && !node.condition)
            {
                throw std::logic_error("toJson: plan node '" + std::string(opName(node.op)) +
                                       "' has no condition");
            }

            out = PlanJson::object();
            out[kOp] = opName(node.op);
            switch (node.op)
            {
                case PlanOp::Scan:
                    out[kTable] = node.table;
                    return;
                case PlanOp::Select:
                    out[kCondition] = toString(*node.condition, ConditionStyle::Plan);
                    writeNode(node.children[0], out[kChild]);
                    return;
                case PlanOp::Project:
                {
                    PlanJson& columns = out[kColumns];
                    columns = PlanJson::array();
                    for (const ColumnRef& ref : node.columns)
                    {
                        columns.push_back(ref.toString());
                    }
                    writeNode(node.children[0], out[kChild]);
                    return;
                }
                case PlanOp::Join:
                    out[kCondition] = toString(*node.condition, ConditionStyle::Plan);
                    writeNode(node.children[0], out[kLeft]);
                    writeNode(node.children[1], out[kRight]);
                    return;
                case PlanOp::Rename:
                    out[kTo] = node.alias;
                    writeNode(node.children[0], out[kChild]);
                    return;
                case PlanOp::Distinct:
                    writeNode(node.children[0], out[kChild]);
                    return;
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                    writeNode(node.children[0], out[kLeft]);
                    writeNode(node.children[1], out[kRight]);
                    return;
            }
        }

        // --- Reading -------------------------------------------------------

        [[noreturn]] void fail(const std::string& message)
        {
            throw Error(ErrorCategory::Schema, message);
        }

        std::string nodePrefix(const std::string& op)
        {
            return "Node '" + op + "': ";
        }

        const std::string& readName(const PlanJson& json, const char* field, const std::string& op)
        {
            const PlanJson& value = json.at(field);
            if (!value.is_string())
            {
                fail(nodePrefix(op) + "field '" + field + "' must be a string.");
            }
            const std::string& name = value.get_ref<const std::string&>();
            if (name.empty())
            {
                fail(nodePrefix(op) + "field '" + field + "' must not be empty.");
            }
            return name;
        }

        // A condition inside a plan continues the plan's depth count (the
        // limits.h rule), so it is parsed through the same seam as the RA
        // parser uses, starting at the level of its node.
        Condition readCondition(const PlanJson& json, const std::string& op, int level)
        {
            const PlanJson& value = json.at(kCondition);
            if (!value.is_string())
            {
                fail(nodePrefix(op) + "field 'condition' must be a string.");
            }
            const std::string& text = value.get_ref<const std::string&>();
            try
            {
                const std::vector<Token> tokens = tokenize(text);
                std::size_t position = 0;
                detail::ParsedCondition parsed = detail::parseConditionAt(tokens, position, level);
                if (tokens[position].kind != TokenKind::End)
                {
                    detail::throwSyntaxError(tokens[position], "Expected end of condition");
                }
                return std::move(parsed.condition);
            }
            catch (const Error& error)
            {
                // A position inside the condition string is not a position in
                // the JSON text, so it moves into the message.
                std::string message = nodePrefix(op) + "invalid condition \"" + text + "\": " + error.message();
                if (error.line() != 0)
                {
                    message += " (line " + std::to_string(error.line()) + ", column " +
                               std::to_string(error.column()) + " of the condition)";
                }
                throw Error(error.category(), message);
            }
        }

        // A column string uses the condition grammar's column_ref rule:
        // `name` or `qualifier.name`.
        ColumnRef readColumn(const std::string& text, const std::string& op)
        {
            const std::string invalid = nodePrefix(op) + "invalid column '" + text + "'.";
            std::vector<Token> tokens;
            try
            {
                tokens = tokenize(text);
            }
            catch (const Error&)
            {
                fail(invalid);
            }
            if (tokens.size() == 2 && detail::isNameToken(tokens[0].kind))
            {
                return ColumnRef{std::nullopt, tokens[0].text};
            }
            if (tokens.size() == 4 && detail::isNameToken(tokens[0].kind) && tokens[1].kind == TokenKind::Dot &&
                detail::isNameToken(tokens[2].kind))
            {
                return ColumnRef{tokens[0].text, tokens[2].text};
            }
            fail(invalid);
        }

        std::vector<ColumnRef> readColumns(const PlanJson& json, const std::string& op)
        {
            const PlanJson& value = json.at(kColumns);
            if (!value.is_array())
            {
                fail(nodePrefix(op) + "field 'columns' must be an array of strings.");
            }
            if (value.empty())
            {
                fail(nodePrefix(op) + "field 'columns' must not be empty.");
            }
            std::vector<ColumnRef> columns;
            for (const PlanJson& item : value)
            {
                if (!item.is_string())
                {
                    fail(nodePrefix(op) + "field 'columns' must be an array of strings.");
                }
                columns.push_back(readColumn(item.get_ref<const std::string&>(), op));
            }
            return columns;
        }

        // Checks `op` and the field set of one node; returns its op.
        PlanOp readHeader(const PlanJson& json)
        {
            const auto opField = json.find(kOp);
            if (opField == json.end())
            {
                fail("Plan node is missing required field 'op'.");
            }
            if (!opField->is_string())
            {
                fail("Plan node field 'op' must be a string.");
            }
            const std::string& name = opField->get_ref<const std::string&>();
            const std::optional<PlanOp> op = opFromName(name);
            if (!op)
            {
                fail("Unknown plan operator '" + name + "'.");
            }

            const std::vector<const char*> fields = fieldsOf(*op);
            for (const char* field : fields)
            {
                if (!json.contains(field))
                {
                    fail(nodePrefix(name) + "missing required field '" + field + "'.");
                }
            }
            for (const auto& item : json.items())
            {
                bool known = item.key() == kOp;
                for (const char* field : fields)
                {
                    known = known || item.key() == field;
                }
                if (!known)
                {
                    fail(nodePrefix(name) + "unexpected field '" + item.key() + "'.");
                }
            }
            return *op;
        }

        // Reads one node into `out`. `parentLevel` counts the operator nodes
        // above it; the check comes before any recursion, so a too-deep plan
        // is rejected without walking it.
        void readNode(const PlanJson& json, int parentLevel, PlanNode& out);

        void readInput(const PlanJson& parent, const char* field, const std::string& op, int level, PlanNode& out)
        {
            const PlanJson& input = parent.at(field);
            if (!input.is_object())
            {
                fail(nodePrefix(op) + "field '" + field + "' must be an object.");
            }
            readNode(input, level, out);
        }

        void readNode(const PlanJson& json, int parentLevel, PlanNode& out)
        {
            out.op = readHeader(json);
            const std::string& op = json.at(kOp).get_ref<const std::string&>();
            const int level = parentLevel + (out.op == PlanOp::Scan ? 0 : 1);
            if (level > kMaxNestingDepth)
            {
                fail(nodePrefix(op) + "plan nested deeper than " + std::to_string(kMaxNestingDepth) + " levels.");
            }

            switch (out.op)
            {
                case PlanOp::Scan:
                    out.table = readName(json, kTable, op);
                    break;
                case PlanOp::Select:
                case PlanOp::Join:
                    out.condition = readCondition(json, op, level);
                    break;
                case PlanOp::Project:
                    out.columns = readColumns(json, op);
                    break;
                case PlanOp::Rename:
                    out.alias = readName(json, kTo, op);
                    break;
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                case PlanOp::Distinct:
                    break;
            }

            out.children.resize(inputCount(out.op));
            if (out.children.size() == 1)
            {
                readInput(json, kChild, op, level, out.children[0]);
            }
            else if (out.children.size() == 2)
            {
                readInput(json, kLeft, op, level, out.children[0]);
                readInput(json, kRight, op, level, out.children[1]);
            }
        }

        // nlohmann reports the byte where parsing stopped; turn it into the
        // 1-based line and column of that byte.
        std::pair<int, int> lineAndColumn(std::string_view text, std::size_t byte)
        {
            int line = 1;
            int column = 1;
            const std::size_t end = byte == 0 ? 0 : std::min(byte - 1, text.size());
            for (std::size_t i = 0; i < end; ++i)
            {
                if (text[i] == '\n')
                {
                    ++line;
                    column = 1;
                }
                else
                {
                    ++column;
                }
            }
            return {line, column};
        }

        // Drops nlohmann's "[json.exception.parse_error.101] parse error at
        // line L, column C: " prefix; the position is reported separately.
        std::string parseErrorDetail(const std::string& what)
        {
            std::string detail = what;
            const std::size_t bracket = detail.find("] ");
            if (bracket != std::string::npos)
            {
                detail.erase(0, bracket + 2);
            }
            const std::string positionPrefix = "parse error at line";
            if (detail.compare(0, positionPrefix.size(), positionPrefix) == 0)
            {
                const std::size_t colon = detail.find(": ");
                if (colon != std::string::npos)
                {
                    detail.erase(0, colon + 2);
                }
            }
            return detail;
        }

    } // namespace

    PlanJson toJson(const PlanNode& plan)
    {
        PlanJson json;
        writeNode(plan, json);
        return json;
    }

    PlanNode fromJson(const PlanJson& json)
    {
        if (!json.is_object())
        {
            fail("Plan must be a JSON object.");
        }
        PlanNode plan;
        readNode(json, 0, plan);
        return plan;
    }

    std::string toJsonText(const PlanNode& plan, int indent)
    {
        try
        {
            return toJson(plan).dump(indent < 0 ? -1 : indent);
        }
        catch (const PlanJson::type_error&)
        {
            // dump() is the one place that requires valid UTF-8.
            throw Error(ErrorCategory::Schema, "Plan cannot be written as JSON: a string is not valid UTF-8.");
        }
    }

    PlanNode fromJsonText(std::string_view text)
    {
        PlanJson json;
        try
        {
            json = PlanJson::parse(text.begin(), text.end());
        }
        catch (const PlanJson::parse_error& error)
        {
            const auto [line, column] = lineAndColumn(text, error.byte);
            throw Error(ErrorCategory::Syntax, "Malformed plan JSON: " + parseErrorDetail(error.what()), line, column);
        }
        return fromJson(json);
    }

} // namespace db
