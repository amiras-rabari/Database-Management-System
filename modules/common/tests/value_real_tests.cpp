// Exec chunk 10 additions to common: the REAL value (AVG's result only,
// never a table column) and the aggregate output names (SPEC §M2.3).
// Decisions: DECISIONS.md 2026-11-07 (REAL, AVG rounding, aggregate names).

#include <optional>
#include <string>

#include <doctest/doctest.h>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/evaluator.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/value.h"

namespace
{
    using db::Aggregate;
    using db::AggregateFunction;
    using db::ColumnRef;
    using db::ErrorCategory;
    using db::Type;
    using db::Value;

    // The category of the db::Error that `function` throws, or nullopt.
    template <typename Function>
    std::optional<ErrorCategory> errorCategoryOf(Function function)
    {
        try
        {
            function();
        }
        catch (const db::Error& error)
        {
            return error.category();
        }
        return std::nullopt;
    }
} // namespace

TEST_CASE("REAL prints with exactly four decimals")
{
    CHECK(db::typeName(Type::Real) == "REAL");
    CHECK(Value::ofReal(7.0 / 2).toText() == "3.5000");
    CHECK(Value::ofReal(1.0 / 3).toText() == "0.3333");
    CHECK(Value::ofReal(2.0 / 3).toText() == "0.6667");
    CHECK(Value::ofReal(-7.0 / 2).toText() == "-3.5000");
    CHECK(Value::ofReal(-1.0 / 3).toText() == "-0.3333");
    CHECK(Value::ofReal(5.0 / 1).toText() == "5.0000");

    const Value v = Value::ofReal(150.0);
    CHECK(v.type() == Type::Real);
    CHECK(v.asReal() == 150.0);
}

TEST_CASE("REAL compares with REAL and INT as numbers, never with VARCHAR")
{
    CHECK(db::compareValues(Value::ofReal(1.5), Value::ofReal(2.5)) < 0);
    CHECK(db::compareValues(Value::ofReal(2.5), Value::ofReal(2.5)) == 0);
    // HAVING AVG(salary) > 50000 compares a REAL with an INT literal.
    CHECK(db::compareValues(Value::ofReal(150.5), Value::ofInt(100)) > 0);
    CHECK(db::compareValues(Value::ofInt(150), Value::ofReal(150.0)) == 0);
    CHECK(db::compareValues(Value::ofInt(-3), Value::ofReal(-2.5)) < 0);

    CHECK(errorCategoryOf([] { db::compareValues(Value::ofReal(1.0), Value::ofVarchar("1")); }) ==
          ErrorCategory::Type);
    CHECK(errorCategoryOf([] { db::compareValues(Value::ofVarchar("1"), Value::ofReal(1.0)); }) ==
          ErrorCategory::Type);
    // The old rule is unchanged.
    CHECK(errorCategoryOf([] { db::compareValues(Value::ofInt(1), Value::ofVarchar("1")); }) ==
          ErrorCategory::Type);
}

TEST_CASE("a condition binds a REAL column against an INT literal, not a VARCHAR one")
{
    const db::Schema schema({{"Emp", "did", Type::Int, 0}, {"", "avg_Emp_salary", Type::Real, 0}});
    const db::BoundCondition above = db::bindCondition(db::parseCondition("avg_Emp_salary > 50000"), schema);
    CHECK(above.evaluate({Value::ofInt(1), Value::ofReal(50000.5)}));
    CHECK_FALSE(above.evaluate({Value::ofInt(1), Value::ofReal(50000.0)}));

    CHECK(errorCategoryOf([&] { db::bindCondition(db::parseCondition("avg_Emp_salary = 'x'"), schema); }) ==
          ErrorCategory::Type);
}

TEST_CASE("aggregate output names follow the spec's example")
{
    CHECK(db::aggregateOutputName(Aggregate{AggregateFunction::Count, std::nullopt, ""}) == "count_star");
    CHECK(db::aggregateOutputName(Aggregate{AggregateFunction::Avg, ColumnRef{"R", "salary"}, ""}) == "avg_R_salary");
    CHECK(db::aggregateOutputName(Aggregate{AggregateFunction::Max, ColumnRef{"R", "name"}, ""}) == "max_R_name");
    CHECK(db::aggregateOutputName(Aggregate{AggregateFunction::Sum, ColumnRef{std::nullopt, "a"}, ""}) == "sum_a");
    CHECK(db::aggregateOutputName(Aggregate{AggregateFunction::Min, ColumnRef{"S", "b"}, ""}) == "min_S_b");
    CHECK(db::aggregateOutputName(Aggregate{AggregateFunction::Count, ColumnRef{"R", "a"}, ""}) == "count_R_a");
}
