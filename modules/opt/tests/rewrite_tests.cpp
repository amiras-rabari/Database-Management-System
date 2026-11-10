// Chunks 06-08: the seven rewrite rules of §M4.3 and the worked example.
//
// Tables: Emp(eid, name, did, age, salary), Dept(did, dname, budget),
// R(a, b), S(c, d); all INT except name and dname. With this column order
// the projections rule 6 inserts read exactly like the spec's text.
// Expected trees are built with common's builders and compared with ==.

#include <doctest/doctest.h>

#include <db/common/condition.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/opt/rewrite.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using db::Aggregate;
    using db::AggregateFunction;
    using db::ColumnRef;
    using db::PlanNode;
    using db::Schema;
    using db::Type;
    using db::opt::RuleSet;

    std::optional<Schema> tableSchema(const std::string& name)
    {
        if (name == "Emp")
        {
            return Schema({{"Emp", "eid", Type::Int, 0},
                           {"Emp", "name", Type::Varchar, 20},
                           {"Emp", "did", Type::Int, 0},
                           {"Emp", "age", Type::Int, 0},
                           {"Emp", "salary", Type::Int, 0}});
        }
        if (name == "Dept")
        {
            return Schema({{"Dept", "did", Type::Int, 0},
                           {"Dept", "dname", Type::Varchar, 20},
                           {"Dept", "budget", Type::Int, 0}});
        }
        if (name == "R")
        {
            return Schema({{"R", "a", Type::Int, 0}, {"R", "b", Type::Int, 0}});
        }
        if (name == "S")
        {
            return Schema({{"S", "c", Type::Int, 0}, {"S", "d", Type::Int, 0}});
        }
        return std::nullopt;
    }

    // Only the listed rules on (numbers as in §M4.3).
    RuleSet only(std::initializer_list<int> numbers)
    {
        RuleSet rules{};
        for (int n : numbers)
        {
            rules[n - 1] = true;
        }
        return rules;
    }

    // Every rule on except the listed ones.
    RuleSet allBut(std::initializer_list<int> numbers)
    {
        RuleSet rules = db::opt::kAllRules;
        for (int n : numbers)
        {
            rules[n - 1] = false;
        }
        return rules;
    }

    PlanNode rewrite(const PlanNode& plan, const RuleSet& rules)
    {
        return db::opt::rewritePlan(plan, rules, tableSchema);
    }

    PlanNode sel(const std::string& condition, PlanNode child)
    {
        return PlanNode::select(db::parseCondition(condition), std::move(child));
    }

    PlanNode joinOn(const std::string& condition, PlanNode left, PlanNode right)
    {
        return PlanNode::join(db::parseCondition(condition), std::move(left), std::move(right));
    }

    PlanNode proj(std::vector<ColumnRef> columns, PlanNode child)
    {
        return PlanNode::project(std::move(columns), std::move(child));
    }

    ColumnRef col(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    PlanNode empE()
    {
        return PlanNode::rename("e", PlanNode::scan("Emp"));
    }

    PlanNode deptD()
    {
        return PlanNode::rename("d", PlanNode::scan("Dept"));
    }

    // The §M4.3 query as sql translates it: one select with the whole
    // WHERE over the cross, project on top.
    PlanNode workedExampleInput()
    {
        return proj({col("e", "name")},
                    sel("e.did = d.did and e.age > 60 and d.budget > 1000", PlanNode::cross(empE(), deptD())));
    }

    // The spec's "after" tree without its two projections (rules 1-3).
    PlanNode afterWithoutProjections()
    {
        return proj({col("e", "name")},
                    joinOn("e.did = d.did", sel("e.age > 60", empE()), sel("d.budget > 1000", deptD())));
    }

    Aggregate countStar()
    {
        return Aggregate{AggregateFunction::Count, std::nullopt, "n"};
    }

    // Compares two plans. The one-line text form shows both trees when they
    // differ; == also catches what the text does not print.
    void checkPlan(const PlanNode& actual, const PlanNode& expected)
    {
        CHECK(db::toString(actual) == db::toString(expected));
        CHECK((actual == expected));
    }
} // namespace

// ---------------------------------------------------------------------------
// Chunk 06: rules 1-3
// ---------------------------------------------------------------------------

TEST_CASE("rule 1 alone gives the §M4.3 'before' tree")
{
    PlanNode before = proj({col("e", "name")},
                           sel("e.did = d.did",
                               sel("e.age > 60", sel("d.budget > 1000", PlanNode::cross(empE(), deptD())))));
    checkPlan(rewrite(workedExampleInput(), only({1})), before);
}

TEST_CASE("rules 1-3: single-sided predicates go to their side, the equality makes a join")
{
    checkPlan(rewrite(workedExampleInput(), only({1, 2, 3})), afterWithoutProjections());
    // Rules 4 and 5 change nothing here (a rename over a bare scan is skipped).
    checkPlan(rewrite(workedExampleInput(), only({1, 2, 3, 4, 5})), afterWithoutProjections());
}

TEST_CASE("comma join and JOIN ON give the same tree")
{
    PlanNode joinOnForm = proj({col("e", "name")},
                               sel("d.budget > 1000", joinOn("e.did = d.did and e.age > 60", empE(), deptD())));
    checkPlan(rewrite(joinOnForm, only({1, 2, 3})), afterWithoutProjections());
    checkPlan(rewrite(joinOnForm, db::opt::kAllRules), rewrite(workedExampleInput(), db::opt::kAllRules));
}

TEST_CASE("preconditions: what must not move")
{
    // Both sides, not an equality: neither rule 2 nor rule 3 applies.
    PlanNode bothSides = sel("e.age < d.budget", PlanNode::cross(empE(), deptD()));
    checkPlan(rewrite(bothSides, db::opt::kAllRules), bothSides);

    // A constant uses no column: no side to push it to.
    PlanNode constant = sel("1 = 1", PlanNode::cross(empE(), deptD()));
    checkPlan(rewrite(constant, db::opt::kAllRules), constant);

    // Rule 1 off: the whole AND uses both sides and is no equality, so it
    // cannot move; rule 6 still narrows the cross inputs to what it reads.
    PlanNode whole = rewrite(workedExampleInput(), allBut({1}));
    checkPlan(whole, proj({col("e", "name")},
                          sel("e.did = d.did and e.age > 60 and d.budget > 1000",
                              PlanNode::cross(proj({col("e", "name"), col("e", "did"), col("e", "age")}, empE()),
                                              proj({col("d", "did"), col("d", "budget")}, deptD())))));

    // Rule 2 off: the join still forms, the single-sided predicate stays above.
    PlanNode input = sel("e.did = d.did and e.age > 60", PlanNode::cross(empE(), deptD()));
    checkPlan(rewrite(input, only({1, 3})), sel("e.age > 60", joinOn("e.did = d.did", empE(), deptD())));

    // Rule 3 off: the cross stays a cross...
    checkPlan(rewrite(input, only({1, 2})), sel("e.did = d.did", PlanNode::cross(sel("e.age > 60", empE()), deptD())));
    // ...and a written join stays a join (no select + cross normalisation).
    PlanNode written = sel("e.age > 60", joinOn("e.did = d.did", empE(), deptD()));
    checkPlan(rewrite(written, only({1, 2})), joinOn("e.did = d.did", sel("e.age > 60", empE()), deptD()));
}

TEST_CASE("the order the conditions are written in does not matter")
{
    PlanNode reordered = proj({col("e", "name")}, sel("d.budget > 1000 and e.age > 60 and e.did = d.did",
                                                      PlanNode::cross(empE(), deptD())));
    checkPlan(rewrite(reordered, only({1, 2, 3})), afterWithoutProjections());
}

TEST_CASE("all rules off changes nothing; the input is never modified")
{
    PlanNode input = workedExampleInput();
    PlanNode copy = input;
    checkPlan(rewrite(input, RuleSet{}), input);
    rewrite(input, db::opt::kAllRules);
    checkPlan(input, copy);
}

TEST_CASE("a 400-conjunct AND chain over a cross")
{
    std::string text = "e.age > 0";
    for (int i = 1; i < 400; ++i)
    {
        text += (i % 2 == 0) ? " and e.age > " + std::to_string(i) : " and d.budget > " + std::to_string(i);
    }
    PlanNode input = sel(text, PlanNode::cross(empE(), deptD()));

    // Every conjunct is single-sided, so all of them leave the cross.
    PlanNode merged = rewrite(input, db::opt::kAllRules);
    REQUIRE((merged.op == db::PlanOp::Cross));
    CHECK((merged.children[0].op == db::PlanOp::Select));
    CHECK((merged.children[1].op == db::PlanOp::Select));

    // Without rule 7 they stay as 200 stacked selects on each side.
    PlanNode stacked = rewrite(input, allBut({7}));
    REQUIRE((stacked.op == db::PlanOp::Cross));
    int depth = 0;
    for (const PlanNode* node = &stacked.children[0]; node->op == db::PlanOp::Select; node = &node->children[0])
    {
        ++depth;
    }
    CHECK((depth == 200));
}

// ---------------------------------------------------------------------------
// Chunk 07: rules 4 and 5
// ---------------------------------------------------------------------------

TEST_CASE("rule 4: below a rename over a derived table, names rewritten")
{
    PlanNode derived = proj({col("R", "a"), col("R", "b")}, PlanNode::scan("R"));
    PlanNode input = sel("t.a > 5", PlanNode::rename("t", derived));
    checkPlan(rewrite(input, only({1, 4})), PlanNode::rename("t", sel("R.a > 5", derived)));
}

TEST_CASE("rule 4: not below a rename over a bare scan (the §M4.3 example keeps it above)")
{
    PlanNode input = sel("e.age > 60", empE());
    checkPlan(rewrite(input, db::opt::kAllRules), input);
}

TEST_CASE("rule 4: into BOTH branches of union and union_all")
{
    PlanNode r = PlanNode::scan("R");
    PlanNode s = PlanNode::scan("S");

    // The union's columns are the left side's names (R.a); the right branch
    // gets the same predicate on its own column at that position (S.c).
    checkPlan(rewrite(sel("R.a > 5", PlanNode::unionOf(r, s)), only({1, 4}))
         , PlanNode::unionOf(sel("R.a > 5", r), sel("S.c > 5", s)));
    checkPlan(rewrite(sel("R.a > 5", PlanNode::unionAll(r, s)), only({1, 4}))
         , PlanNode::unionAll(sel("R.a > 5", r), sel("S.c > 5", s)));

    // Rule 4 off: nothing moves.
    PlanNode input = sel("R.a > 5", PlanNode::unionOf(r, s));
    checkPlan(rewrite(input, allBut({4})), input);
}

TEST_CASE("rule 5: below a group_by only on grouping columns")
{
    PlanNode r = PlanNode::scan("R");
    auto groupByA = [&r](PlanNode child) { return PlanNode::groupBy({col("R", "a")}, {countStar()}, child); };

    // A key predicate filters before grouping (WHERE instead of HAVING).
    checkPlan(rewrite(sel("R.a = 3", groupByA(r)), only({1, 5})), groupByA(sel("R.a = 3", r)));

    // A predicate on an aggregate can never move: n does not exist below.
    PlanNode having = sel("n > 5", groupByA(r));
    checkPlan(rewrite(having, db::opt::kAllRules), having);

    // Mixed: rule 1 splits it, only the key part moves.
    checkPlan(rewrite(sel("R.a = 3 and n > 5", groupByA(r)), only({1, 5})), sel("n > 5", groupByA(sel("R.a = 3", r))));

    // Rule 5 off: nothing moves.
    PlanNode input = sel("R.a = 3", groupByA(r));
    checkPlan(rewrite(input, allBut({5})), input);
}

TEST_CASE("rule 5: a constant never goes below a group_by")
{
    // With no keys, an empty input still gives one row (count = 0). Below,
    // `1 = 2` would leave that row; above, it removes it.
    PlanNode noKeys = sel("1 = 2", PlanNode::groupBy({}, {countStar()}, PlanNode::scan("R")));
    checkPlan(rewrite(noKeys, db::opt::kAllRules), noKeys);

    PlanNode withKeys = sel("1 = 2", PlanNode::groupBy({col("R", "a")}, {countStar()}, PlanNode::scan("R")));
    checkPlan(rewrite(withKeys, db::opt::kAllRules), withKeys);
}

TEST_CASE("selects are not pushed below limit, sort, distinct or project")
{
    PlanNode r = PlanNode::scan("R");
    for (const PlanNode& child : {PlanNode::limit(3, r), PlanNode::sort({db::SortKey{col("R", "a"), true}}, r),
                                  PlanNode::distinct(r), proj({col("R", "b"), col("R", "a")}, r)})
    {
        PlanNode input = sel("R.a > 5", child);
        checkPlan(rewrite(input, db::opt::kAllRules), input);
    }
}

// ---------------------------------------------------------------------------
// Chunk 08: rules 6 and 7, the full worked example
// ---------------------------------------------------------------------------

TEST_CASE("§M4.3 worked example: all rules give the spec's 'after' tree")
{
    PlanNode after = proj({col("e", "name")},
                          joinOn("e.did = d.did",
                                 proj({col("e", "name"), col("e", "did")}, sel("e.age > 60", empE())),
                                 proj({col("d", "did")}, sel("d.budget > 1000", deptD()))));
    PlanNode input = workedExampleInput();
    PlanNode copy = input;

    checkPlan(rewrite(input, db::opt::kAllRules), after);
    checkPlan(input, copy); // the "before" tree is still there for explain

    // Rule 6 off: no projections.
    checkPlan(rewrite(input, allBut({6})), afterWithoutProjections());
}

TEST_CASE("rule 6 keeps every column a predicate above still uses")
{
    PlanNode input =
        proj({col("e", "name")}, sel("e.age > d.budget", joinOn("e.did = d.did", empE(), deptD())));
    PlanNode expected = proj({col("e", "name")},
                             sel("e.age > d.budget",
                                 joinOn("e.did = d.did", proj({col("e", "name"), col("e", "did"), col("e", "age")}, empE()),
                                        proj({col("d", "did"), col("d", "budget")}, deptD()))));
    checkPlan(rewrite(input, db::opt::kAllRules), expected);
}

TEST_CASE("rule 6 under a distinct keeps the columns the distinct and the join need")
{
    PlanNode input = PlanNode::distinct(
        proj({col("e", "name"), col("d", "dname")}, joinOn("e.did = d.did", empE(), deptD())));
    PlanNode expected = PlanNode::distinct(
        proj({col("e", "name"), col("d", "dname")},
             joinOn("e.did = d.did", proj({col("e", "name"), col("e", "did")}, empE()),
                    proj({col("d", "did"), col("d", "dname")}, deptD()))));
    checkPlan(rewrite(input, db::opt::kAllRules), expected);
}

TEST_CASE("rule 6 never narrows the input of a distinct")
{
    // Only e.name is used above the distinct, but dropping columns below it
    // would change which rows count as duplicates. So both join inputs keep
    // every column, and nothing is inserted.
    PlanNode input = proj({col("e", "name")}, PlanNode::distinct(joinOn("e.did = d.did", empE(), deptD())));
    checkPlan(rewrite(input, db::opt::kAllRules), input);
}

TEST_CASE("rule 6 keeps sort keys and aggregate inputs")
{
    PlanNode sorted = proj({col("e", "name")},
                           PlanNode::sort({db::SortKey{col("e", "age"), true}}, joinOn("e.did = d.did", empE(), deptD())));
    // Left keeps name (the project above), did (the join) and age (the sort key).
    PlanNode sortedAfter = proj({col("e", "name")},
                                PlanNode::sort({db::SortKey{col("e", "age"), true}},
                                               joinOn("e.did = d.did",
                                                      proj({col("e", "name"), col("e", "did"), col("e", "age")}, empE()),
                                                      proj({col("d", "did")}, deptD()))));
    checkPlan(rewrite(sorted, db::opt::kAllRules), sortedAfter);

    Aggregate avgSalary{AggregateFunction::Avg, col("e", "salary"), "a"};
    PlanNode grouped = PlanNode::groupBy({col("d", "dname")}, {avgSalary}, joinOn("e.did = d.did", empE(), deptD()));
    PlanNode groupedAfter = PlanNode::groupBy(
        {col("d", "dname")}, {avgSalary},
        joinOn("e.did = d.did", proj({col("e", "did"), col("e", "salary")}, empE()),
               proj({col("d", "did"), col("d", "dname")}, deptD())));
    checkPlan(rewrite(grouped, db::opt::kAllRules), groupedAfter);
}

TEST_CASE("rule 6 inserts nothing when a side needs no column at all")
{
    // count(*) over a cross needs no column; a projection of zero columns
    // would not be a valid plan, so nothing is inserted.
    PlanNode input = PlanNode::groupBy({}, {countStar()}, PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("S")));
    checkPlan(rewrite(input, db::opt::kAllRules), input);
}

TEST_CASE("rule 7: a projection that drops nothing goes; a reordering one stays")
{
    PlanNode r = PlanNode::scan("R");
    checkPlan(rewrite(proj({col("R", "a"), col("R", "b")}, r), only({7})), r);

    PlanNode reorder = proj({col("R", "b"), col("R", "a")}, r);
    checkPlan(rewrite(reorder, only({7})), reorder);

    PlanNode keepAll = proj({col("R", "a"), col("R", "b")}, r);
    checkPlan(rewrite(keepAll, allBut({7})), keepAll);
}

TEST_CASE("rule 7: adjacent selects merge into one, in order")
{
    PlanNode input = sel("R.a > 1", sel("R.b > 2", PlanNode::scan("R")));
    checkPlan(rewrite(input, only({7})), sel("R.a > 1 and R.b > 2", PlanNode::scan("R")));
    checkPlan(rewrite(input, allBut({7})), input);
}
