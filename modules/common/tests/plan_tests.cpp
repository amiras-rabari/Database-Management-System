// The plan tree: named constructors, opName, operator==, toString in both
// condition styles, and outputSchema with its errors.

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/common/ra_parser.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace
{

    using db::Column;
    using db::ColumnRef;
    using db::ConditionStyle;
    using db::ErrorCategory;
    using db::PlanNode;
    using db::PlanOp;
    using db::Schema;
    using db::Type;

    // Runs `function` and returns the db::Error it threw, or nullopt if it
    // threw nothing.
    template <typename Function>
    std::optional<db::Error> errorFrom(Function function)
    {
        try
        {
            function();
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    ColumnRef bare(const std::string& name)
    {
        return ColumnRef{std::nullopt, name};
    }

    ColumnRef qualified(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    PlanNode scan(const std::string& table)
    {
        return PlanNode::scan(table);
    }

    // The Bonus 1 Emp/Dept relations, plus R(a, b), S(c, d, e) and
    // T(x VARCHAR, y INT) for the set operations.
    std::optional<Schema> lookup(const std::string& table)
    {
        static const std::map<std::string, Schema> tables = {
            {"Emp", Schema({Column{"Emp", "EID", Type::Varchar, 2}, Column{"Emp", "Name", Type::Varchar, 5},
                            Column{"Emp", "Age", Type::Int, 0}, Column{"Emp", "DID", Type::Varchar, 2},
                            Column{"Emp", "MgrID", Type::Varchar, 2}})},
            {"Dept", Schema({Column{"Dept", "DID", Type::Varchar, 2}, Column{"Dept", "DName", Type::Varchar, 5},
                             Column{"Dept", "Budget", Type::Int, 0}})},
            {"R", Schema({Column{"R", "a", Type::Int, 0}, Column{"R", "b", Type::Int, 0}})},
            {"S", Schema({Column{"S", "c", Type::Int, 0}, Column{"S", "d", Type::Int, 0},
                          Column{"S", "e", Type::Int, 0}})},
            {"T", Schema({Column{"T", "x", Type::Varchar, 3}, Column{"T", "y", Type::Int, 0}})},
        };
        const auto found = tables.find(table);
        if (found == tables.end())
        {
            return std::nullopt;
        }
        return found->second;
    }

    Schema schemaOf(const PlanNode& plan)
    {
        return db::outputSchema(plan, lookup);
    }

    // "Emp.EID VARCHAR, Emp.Age INT, ..." in schema order.
    std::string describe(const Schema& schema)
    {
        std::string out;
        for (const Column& column : schema.columns())
        {
            if (!out.empty())
            {
                out += ", ";
            }
            out += column.qualifiedName() + " " + std::string(db::typeName(column.type));
        }
        return out;
    }

    std::string describePlan(const std::string& query)
    {
        return describe(schemaOf(db::parseRaQuery(query)));
    }

    std::optional<db::Error> schemaError(const PlanNode& plan)
    {
        return errorFrom([&] { schemaOf(plan); });
    }

    std::optional<db::Error> schemaError(const std::string& query)
    {
        return schemaError(db::parseRaQuery(query));
    }

    const char* const kEmp = "Emp.EID VARCHAR, Emp.Name VARCHAR, Emp.Age INT, Emp.DID VARCHAR, Emp.MgrID VARCHAR";
    const char* const kDept = "Dept.DID VARCHAR, Dept.DName VARCHAR, Dept.Budget INT";

} // namespace

// --- Construction -----------------------------------------------------------

TEST_CASE("named constructors set the op, the node data and the children in order")
{
    const db::Condition condition = db::parseCondition("a = 1");

    const PlanNode scanNode = PlanNode::scan("R");
    CHECK(scanNode.op == PlanOp::Scan);
    CHECK(scanNode.table == "R");
    CHECK(scanNode.children.empty());

    const PlanNode selectNode = PlanNode::select(condition, scan("R"));
    CHECK(selectNode.op == PlanOp::Select);
    REQUIRE(selectNode.condition.has_value());
    CHECK(*selectNode.condition == condition);
    REQUIRE(selectNode.children.size() == 1);
    CHECK(selectNode.children[0] == scan("R"));

    const PlanNode projectNode = PlanNode::project({bare("a"), qualified("R", "b")}, scan("R"));
    CHECK(projectNode.op == PlanOp::Project);
    CHECK(projectNode.columns == std::vector<ColumnRef>{bare("a"), qualified("R", "b")});
    CHECK(projectNode.children.size() == 1);

    const PlanNode joinNode = PlanNode::join(condition, scan("R"), scan("S"));
    CHECK(joinNode.op == PlanOp::Join);
    REQUIRE(joinNode.condition.has_value());
    CHECK(*joinNode.condition == condition);
    REQUIRE(joinNode.children.size() == 2);
    CHECK(joinNode.children[0] == scan("R"));
    CHECK(joinNode.children[1] == scan("S"));

    const PlanNode renameNode = PlanNode::rename("E2", scan("Emp"));
    CHECK(renameNode.op == PlanOp::Rename);
    CHECK(renameNode.alias == "E2");
    CHECK(renameNode.children.size() == 1);

    CHECK(PlanNode::cross(scan("R"), scan("S")).op == PlanOp::Cross);
    CHECK(PlanNode::unionOf(scan("R"), scan("S")).op == PlanOp::Union);
    CHECK(PlanNode::intersect(scan("R"), scan("S")).op == PlanOp::Intersect);
    CHECK(PlanNode::except(scan("R"), scan("S")).op == PlanOp::Except);
    CHECK(PlanNode::except(scan("R"), scan("S")).children.size() == 2);
    CHECK(PlanNode::distinct(scan("R")).op == PlanOp::Distinct);
    CHECK(PlanNode::distinct(scan("R")).children.size() == 1);
    CHECK_FALSE(PlanNode::cross(scan("R"), scan("S")).condition.has_value());
}

TEST_CASE("opName gives the M3 logical names")
{
    CHECK(db::opName(PlanOp::Scan) == "scan");
    CHECK(db::opName(PlanOp::Select) == "select");
    CHECK(db::opName(PlanOp::Project) == "project");
    CHECK(db::opName(PlanOp::Join) == "join");
    CHECK(db::opName(PlanOp::Cross) == "cross");
    CHECK(db::opName(PlanOp::Rename) == "rename");
    CHECK(db::opName(PlanOp::Union) == "union");
    CHECK(db::opName(PlanOp::Intersect) == "intersect");
    CHECK(db::opName(PlanOp::Except) == "except");
    CHECK(db::opName(PlanOp::Distinct) == "distinct");
}

TEST_CASE("operator== compares shape, op, node data and child order")
{
    const auto tree = [](const std::string& condition)
    {
        return PlanNode::project({bare("a")},
                                 PlanNode::select(db::parseCondition(condition),
                                                  PlanNode::unionOf(scan("R"), PlanNode::rename("X", scan("S")))));
    };
    CHECK(tree("a = 1") == tree("a = 1"));
    CHECK_FALSE(tree("a = 1") == tree("a = 2"));
    CHECK_FALSE(tree("a = 1") == tree("a = 1 and b = 2"));

    CHECK_FALSE(PlanNode::unionOf(scan("R"), scan("S")) == PlanNode::intersect(scan("R"), scan("S")));
    CHECK_FALSE(PlanNode::unionOf(scan("R"), scan("S")) == PlanNode::unionOf(scan("S"), scan("R")));
    CHECK_FALSE(PlanNode::cross(scan("R"), scan("S")) == PlanNode::cross(scan("R"), scan("T")));
    CHECK_FALSE(scan("R") == scan("S"));
    CHECK_FALSE(PlanNode::rename("X", scan("R")) == PlanNode::rename("Y", scan("R")));
    CHECK_FALSE(PlanNode::project({bare("a")}, scan("R")) == PlanNode::project({qualified("R", "a")}, scan("R")));
    CHECK_FALSE(PlanNode::distinct(scan("R")) == scan("R"));
}

TEST_CASE("operator== ignores fields the op does not use")
{
    PlanNode left = PlanNode::cross(scan("R"), scan("S"));
    PlanNode right = left;
    right.alias = "unused";
    right.table = "unused";
    CHECK(left == right);
}

// --- toString ---------------------------------------------------------------

TEST_CASE("toString prints every node type on one line")
{
    const db::Condition condition = db::parseCondition("a = 1 and (b = 2 or c = 'x')");

    CHECK(db::toString(scan("R")) == "scan(R)");
    CHECK(db::toString(PlanNode::select(condition, scan("R"))) == "select[a = 1 and (b = 2 or c = 'x')](scan(R))");
    CHECK(db::toString(PlanNode::project({bare("a"), qualified("R", "b")}, scan("R"))) == "project[a, R.b](scan(R))");
    CHECK(db::toString(PlanNode::rename("E2", scan("Emp"))) == "rename[E2](scan(Emp))");
    CHECK(db::toString(PlanNode::join(db::parseCondition("R.a = S.c"), scan("R"), scan("S"))) ==
          "join[R.a = S.c](scan(R), scan(S))");
    CHECK(db::toString(PlanNode::cross(scan("R"), scan("S"))) == "cross(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::unionOf(scan("R"), scan("S"))) == "union(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::intersect(scan("R"), scan("S"))) == "intersect(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::except(scan("R"), scan("S"))) == "except(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::distinct(scan("R"))) == "distinct(scan(R))");
}

TEST_CASE("toString in explicit style fully parenthesises conditions only")
{
    const db::Condition condition = db::parseCondition("a = 1 and (b = 2 or c = 'x')");
    const ConditionStyle style = ConditionStyle::Explicit;

    CHECK(db::toString(scan("R"), style) == "scan(R)");
    CHECK(db::toString(PlanNode::select(condition, scan("R")), style) ==
          "select[((a = 1) and ((b = 2) or (c = 'x')))](scan(R))");
    CHECK(db::toString(PlanNode::project({bare("a"), qualified("R", "b")}, scan("R")), style) ==
          "project[a, R.b](scan(R))");
    CHECK(db::toString(PlanNode::rename("E2", scan("Emp")), style) == "rename[E2](scan(Emp))");
    CHECK(db::toString(PlanNode::join(db::parseCondition("R.a = S.c"), scan("R"), scan("S")), style) ==
          "join[(R.a = S.c)](scan(R), scan(S))");
    CHECK(db::toString(PlanNode::cross(scan("R"), scan("S")), style) == "cross(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::unionOf(scan("R"), scan("S")), style) == "union(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::intersect(scan("R"), scan("S")), style) == "intersect(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::except(scan("R"), scan("S")), style) == "except(scan(R), scan(S))");
    CHECK(db::toString(PlanNode::distinct(scan("R")), style) == "distinct(scan(R))");
}

TEST_CASE("toString prints nested trees with the condition style applied at every level")
{
    const PlanNode plan = PlanNode::distinct(PlanNode::project(
        {bare("Name")},
        PlanNode::join(db::parseCondition("Emp.DID = Dept.DID and Budget > 500"),
                       PlanNode::select(db::parseCondition("not Age < 30"), scan("Emp")), scan("Dept"))));
    CHECK(db::toString(plan) ==
          "distinct(project[Name](join[Emp.DID = Dept.DID and Budget > 500](select[not Age < 30](scan(Emp)), "
          "scan(Dept))))");
    CHECK(db::toString(plan, ConditionStyle::Explicit) ==
          "distinct(project[Name](join[((Emp.DID = Dept.DID) and (Budget > 500))](select[(not (Age < 30))]"
          "(scan(Emp)), scan(Dept))))");
}

TEST_CASE("toString rejects a malformed node as an internal error")
{
    PlanNode noCondition = scan("R");
    noCondition.op = PlanOp::Select;
    noCondition.children.push_back(scan("R"));
    CHECK_THROWS_AS(db::toString(noCondition), std::logic_error);

    PlanNode oneChild = PlanNode::cross(scan("R"), scan("S"));
    oneChild.children.pop_back();
    CHECK_THROWS_AS(db::toString(oneChild), std::logic_error);
}

// --- outputSchema -----------------------------------------------------------

TEST_CASE("outputSchema of scan is the table's schema")
{
    CHECK(describe(schemaOf(scan("Emp"))) == kEmp);
    CHECK(describe(schemaOf(scan("R"))) == "R.a INT, R.b INT");
}

TEST_CASE("outputSchema of select and distinct is the child's schema")
{
    CHECK(describe(schemaOf(PlanNode::select(db::parseCondition("Age > 30"), scan("Emp")))) == kEmp);
    CHECK(describe(schemaOf(PlanNode::distinct(scan("Emp")))) == kEmp);
}

TEST_CASE("outputSchema of project lists the named columns in the order written")
{
    CHECK(describe(schemaOf(PlanNode::project({bare("Age"), qualified("Emp", "EID")}, scan("Emp")))) ==
          "Emp.Age INT, Emp.EID VARCHAR");
}

TEST_CASE("outputSchema of join and cross is the left columns, then the right ones")
{
    const std::string both = std::string(kEmp) + ", " + kDept;
    CHECK(describe(schemaOf(PlanNode::join(db::parseCondition("Emp.DID = Dept.DID"), scan("Emp"), scan("Dept")))) ==
          both);
    CHECK(describe(schemaOf(PlanNode::cross(scan("Emp"), scan("Dept")))) == both);
    CHECK(describe(schemaOf(PlanNode::cross(scan("R"), scan("S")))) == "R.a INT, R.b INT, S.c INT, S.d INT, S.e INT");
}

TEST_CASE("outputSchema of rename requalifies every column")
{
    CHECK(describe(schemaOf(PlanNode::rename("X", scan("R")))) == "X.a INT, X.b INT");
}

TEST_CASE("outputSchema of a set operation is the left schema")
{
    const PlanNode renamed = PlanNode::rename("X", scan("R"));
    CHECK(describe(schemaOf(PlanNode::unionOf(scan("R"), renamed))) == "R.a INT, R.b INT");
    CHECK(describe(schemaOf(PlanNode::intersect(renamed, scan("R")))) == "X.a INT, X.b INT");
    CHECK(describe(schemaOf(PlanNode::except(scan("R"), renamed))) == "R.a INT, R.b INT");
}

TEST_CASE("outputSchema: rename then join (Bonus 1 case 20) gives E2 columns then Emp columns")
{
    CHECK(describePlan("rename[E2](Emp) join[Emp.MgrID=E2.EID] Emp") ==
          "E2.EID VARCHAR, E2.Name VARCHAR, E2.Age INT, E2.DID VARCHAR, E2.MgrID VARCHAR, " + std::string(kEmp));
}

TEST_CASE("outputSchema works through a deep plan")
{
    CHECK(describePlan("distinct(project[E2.Name, DName](select[Budget > 500](rename[E2](Emp) join[E2.DID = "
                       "Dept.DID] Dept)))") == "E2.Name VARCHAR, Dept.DName VARCHAR");
}

// --- outputSchema errors ----------------------------------------------------

TEST_CASE("outputSchema: an unknown relation is a name error")
{
    const std::optional<db::Error> error = schemaError(scan("Nope"));
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message() == "Unknown relation 'Nope'.");
}

TEST_CASE("outputSchema: an unknown column in a select is a name error")
{
    const std::optional<db::Error> error = schemaError("select[Salary > 3](Emp)");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
}

TEST_CASE("outputSchema: a bare column on both sides of a join is an ambiguity error")
{
    const std::optional<db::Error> joinError = schemaError("Emp join[DID = 'D1'] Dept");
    REQUIRE(joinError.has_value());
    CHECK(joinError->category() == ErrorCategory::Ambiguity);

    const std::optional<db::Error> selectError = schemaError("select[DID = 'D1'](Emp times Dept)");
    REQUIRE(selectError.has_value());
    CHECK(selectError->category() == ErrorCategory::Ambiguity);
}

TEST_CASE("outputSchema: comparing INT with VARCHAR is a type error (Bonus 1 case 22)")
{
    const std::optional<db::Error> error = schemaError("select[Age > '30'](Emp)");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Type);

    const std::optional<db::Error> joinError = schemaError("Emp join[Emp.Age = Dept.DID] Dept");
    REQUIRE(joinError.has_value());
    CHECK(joinError->category() == ErrorCategory::Type);
}

TEST_CASE("outputSchema: a self join without rename is a schema error")
{
    const std::optional<db::Error> joinError = schemaError("Emp join[Emp.MgrID = Emp.EID] Emp");
    REQUIRE(joinError.has_value());
    CHECK(joinError->category() == ErrorCategory::Schema);

    const std::optional<db::Error> crossError = schemaError("Emp times Emp");
    REQUIRE(crossError.has_value());
    CHECK(crossError->category() == ErrorCategory::Schema);
}

TEST_CASE("outputSchema: the same column projected twice is a schema error (Bonus 1 case 24)")
{
    const std::optional<db::Error> error = schemaError("project[Name, Name](Emp)");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
    CHECK(error->message() == "Duplicate projection attribute 'Emp.Name'.");

    const std::optional<db::Error> mixed = schemaError("project[Name, Emp.Name](Emp)");
    REQUIRE(mixed.has_value());
    CHECK(mixed->category() == ErrorCategory::Schema);
}

TEST_CASE("outputSchema: set operations need the same column count (Bonus 1 case 21)")
{
    for (const char* query : {"R union S", "R intersect S", "R minus S"})
    {
        INFO(query);
        const std::optional<db::Error> error = schemaError(query);
        REQUIRE(error.has_value());
        CHECK(error->category() == ErrorCategory::Schema);
    }
}

TEST_CASE("outputSchema: set operations need the same type at each position")
{
    const std::optional<db::Error> error = schemaError("R union T");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Schema);
    CHECK(error->message() ==
          "Relation schemas are not union-compatible for 'union': "
          "column 1 is INT on the left but VARCHAR on the right.");

    const std::optional<db::Error> except = schemaError("T minus R");
    REQUIRE(except.has_value());
    CHECK(except->message().find("'except'") != std::string::npos);
}

TEST_CASE("outputSchema reports the left child's error before the right child's and the node's own")
{
    const std::optional<db::Error> bothUnknown = schemaError(PlanNode::unionOf(scan("X"), scan("Y")));
    REQUIRE(bothUnknown.has_value());
    CHECK(bothUnknown->message() == "Unknown relation 'X'.");

    // The right child's error comes before the node's own count mismatch.
    const std::optional<db::Error> rightFirst = schemaError(PlanNode::unionOf(scan("S"), scan("Y")));
    REQUIRE(rightFirst.has_value());
    CHECK(rightFirst->message() == "Unknown relation 'Y'.");

    // A select's child is checked before its condition.
    const std::optional<db::Error> childFirst = schemaError("select[Salary > 3](Nope)");
    REQUIRE(childFirst.has_value());
    CHECK(childFirst->message() == "Unknown relation 'Nope'.");
}

TEST_CASE("outputSchema rejects a malformed node as an internal error")
{
    PlanNode noCondition = PlanNode::join(db::parseCondition("a = 1"), scan("R"), scan("S"));
    noCondition.condition.reset();
    CHECK_THROWS_AS(schemaOf(noCondition), std::logic_error);
}
