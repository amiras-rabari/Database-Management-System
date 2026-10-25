// The evaluator: bindCondition / bindJoinCondition and evaluate.

#include <optional>
#include <stdexcept>
#include <string>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/evaluator.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace
{

    using db::BoundCondition;
    using db::Column;
    using db::ErrorCategory;
    using db::Schema;
    using db::Tuple;
    using db::Type;
    using db::Value;

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

    // Emp(EID VARCHAR(5), Name VARCHAR(20), Age INT, DID VARCHAR(5))
    Schema empSchema()
    {
        return Schema({Column{"Emp", "EID", Type::Varchar, 5}, Column{"Emp", "Name", Type::Varchar, 20},
                       Column{"Emp", "Age", Type::Int, 0}, Column{"Emp", "DID", Type::Varchar, 5}});
    }

    // Dept(DID VARCHAR(5), Budget INT)
    Schema deptSchema()
    {
        return Schema({Column{"Dept", "DID", Type::Varchar, 5}, Column{"Dept", "Budget", Type::Int, 0}});
    }

    Tuple alice()
    {
        return {Value::ofVarchar("E1"), Value::ofVarchar("Alice"), Value::ofInt(30), Value::ofVarchar("D1")};
    }

    Tuple sales()
    {
        return {Value::ofVarchar("D1"), Value::ofInt(100)};
    }

    Tuple eng()
    {
        return {Value::ofVarchar("D2"), Value::ofInt(2000)};
    }

    bool holds(const std::string& condition, const Tuple& tuple)
    {
        return db::bindCondition(db::parseCondition(condition), empSchema()).evaluate(tuple);
    }

    std::optional<db::Error> bindError(const std::string& condition)
    {
        return errorFrom([&] { db::bindCondition(db::parseCondition(condition), empSchema()); });
    }

} // namespace

TEST_CASE("all six operators on INT")
{
    const Tuple t = alice();
    CHECK(holds("Age = 30", t));
    CHECK_FALSE(holds("Age = 31", t));
    CHECK(holds("Age <> 31", t));
    CHECK_FALSE(holds("Age <> 30", t));
    CHECK(holds("Age < 31", t));
    CHECK_FALSE(holds("Age < 30", t));
    CHECK(holds("Age <= 30", t));
    CHECK_FALSE(holds("Age <= 29", t));
    CHECK(holds("Age > -30", t));
    CHECK_FALSE(holds("Age > 30", t));
    CHECK(holds("Age >= 30", t));
    CHECK_FALSE(holds("Age >= 31", t));
    CHECK(holds("Age = 030", t));
}

TEST_CASE("all six operators on VARCHAR compare byte-wise")
{
    const Tuple t = alice();
    CHECK(holds("Name = 'Alice'", t));
    CHECK_FALSE(holds("Name = 'alice'", t));
    CHECK(holds("Name <> 'Alicia'", t));
    CHECK_FALSE(holds("Name <> 'Alice'", t));
    CHECK(holds("Name < 'Alicia'", t));
    CHECK_FALSE(holds("Name < 'Al'", t));
    CHECK(holds("Name <= 'Alice'", t));
    CHECK_FALSE(holds("Name <= 'Ali'", t));
    CHECK(holds("Name > 'Ali'", t));
    CHECK_FALSE(holds("Name > 'a'", t));
    CHECK(holds("Name >= 'Alice'", t));
    CHECK_FALSE(holds("Name >= 'Bob'", t));
    CHECK(holds("DID < EID", t));
}

TEST_CASE("and, or and not combine comparisons")
{
    const Tuple t = alice();
    CHECK(holds("Age = 30 and Name = 'Alice'", t));
    CHECK_FALSE(holds("Age = 30 and Name = 'Bob'", t));
    CHECK(holds("Age = 31 or Name = 'Alice'", t));
    CHECK_FALSE(holds("Age = 31 or Name = 'Bob'", t));
    CHECK(holds("not Age = 31", t));
    CHECK_FALSE(holds("not Age = 30", t));
    CHECK(holds("not (Age = 31 and Name = 'Alice') or Age > 100", t));
    CHECK_FALSE(holds("not (Age = 30 and Name = 'Alice') or Age > 100", t));
}

TEST_CASE("qualified and bare names resolve to the same column")
{
    CHECK(holds("Emp.Age = 30 and Age = 30", alice()));
}

TEST_CASE("a comparison of two literals binds when the types match")
{
    CHECK(holds("1 = 1", alice()));
    CHECK_FALSE(holds("'a' = 'b'", alice()));
    REQUIRE(bindError("1 = 'a'").has_value());
    CHECK(bindError("1 = 'a'")->category() == ErrorCategory::Type);
}

TEST_CASE("an INT column against a string literal is a type error at bind time (Bonus 1 case 22)")
{
    const std::optional<db::Error> error = bindError("Age > '30'");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Type);
    CHECK(error->message() == "Incompatible comparison operand types: INT and VARCHAR.");
}

TEST_CASE("an INT column against a VARCHAR column is a type error")
{
    const std::optional<db::Error> error = bindError("Name = Age");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Type);
    CHECK(error->message() == "Incompatible comparison operand types: VARCHAR and INT.");
}

TEST_CASE("an unknown column is a name error at bind time")
{
    REQUIRE(bindError("Salary > 3").has_value());
    CHECK(bindError("Salary > 3")->category() == ErrorCategory::Name);
    REQUIRE(bindError("Age = 1 or Dept.DID = 'D1'").has_value());
    CHECK(bindError("Age = 1 or Dept.DID = 'D1'")->category() == ErrorCategory::Name);
}

TEST_CASE("a bare name present on both join sides is an ambiguity error")
{
    const std::optional<db::Error> error =
        errorFrom([] { db::bindJoinCondition(db::parseCondition("DID = 'D1'"), empSchema(), deptSchema()); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Ambiguity);
}

TEST_CASE("a join condition evaluates on separate left and right tuples")
{
    const BoundCondition join =
        db::bindJoinCondition(db::parseCondition("Emp.DID = Dept.DID"), empSchema(), deptSchema());
    CHECK(join.evaluate(alice(), sales()));
    CHECK_FALSE(join.evaluate(alice(), eng()));
}

TEST_CASE("a join condition reads right-side columns from the right tuple")
{
    const BoundCondition join =
        db::bindJoinCondition(db::parseCondition("Dept.Budget > 500 and Age < Budget"), empSchema(), deptSchema());
    CHECK_FALSE(join.evaluate(alice(), sales()));
    CHECK(join.evaluate(alice(), eng()));

    const BoundCondition flipped =
        db::bindJoinCondition(db::parseCondition("Dept.DID = Emp.DID"), empSchema(), deptSchema());
    CHECK(flipped.evaluate(alice(), sales()));
}

TEST_CASE("a join condition type-checks across the two sides")
{
    const std::optional<db::Error> error =
        errorFrom([] { db::bindJoinCondition(db::parseCondition("Emp.Age = Dept.DID"), empSchema(), deptSchema()); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Type);
}

TEST_CASE("evaluating with the wrong number of tuples is an internal error")
{
    const BoundCondition single = db::bindCondition(db::parseCondition("Age = 30"), empSchema());
    const BoundCondition join =
        db::bindJoinCondition(db::parseCondition("Emp.DID = Dept.DID"), empSchema(), deptSchema());
    CHECK_THROWS_AS(single.evaluate(alice(), sales()), std::logic_error);
    CHECK_THROWS_AS(join.evaluate(alice()), std::logic_error);
}

TEST_CASE("a bound condition can be copied and reused")
{
    const BoundCondition original = db::bindCondition(db::parseCondition("Age >= 30"), empSchema());
    const BoundCondition copy = original;
    Tuple younger = alice();
    younger[2] = Value::ofInt(29);
    CHECK(copy.evaluate(alice()));
    CHECK_FALSE(copy.evaluate(younger));
    CHECK(original.evaluate(alice()));
}
