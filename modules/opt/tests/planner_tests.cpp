// Chunk 12: the planner pipeline, optimize() (§M4 intro, §M4.5, §M4.6,
// §M4.7 #2 and #3, §M4.9).
//
// Made-up tables (never on disk) for the shape tests:
// - Emp(name, did, age) 10,000 rows on 200 pages, Dept(did, budget) 50 rows
//   on 1 page, never analyzed (documented defaults).
// - A, Bp, C: the §M4.5 example (B' is written Bp; a quote cannot be part
//   of a name). A(x) 5,000 rows on 100 pages, Bp(x, y) 500 rows on 10
//   pages, C(y) 20,000 rows on 400 pages. ndv(A.x) = 2,500 and
//   ndv(C.y) = 5,000 give the spec's 1,000 and 2,000 joined rows. Every
//   width is 40.96 bytes, so 1,000 rows of A and Bp together (81.92 bytes)
//   fill exactly the spec's 20 pages, and 2,000 rows of Bp and C 40.
// - T1 .. T7(k): seven small tables for the "more than six" rule.
// The exec tests build a small real database.

#include <doctest/doctest.h>

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/plan_json.h>
#include <db/common/schema.h>
#include <db/common/value.h>
#include <db/exec/counters.h>
#include <db/exec/exec_context.h>
#include <db/exec/executable_plan.h>
#include <db/opt/analyze.h>
#include <db/opt/enumerate.h>
#include <db/opt/estimate.h>
#include <db/opt/optimizer.h>
#include <db/opt/physical.h>
#include <db/opt/rewrite.h>
#include <db/storage/heap_file.h>
#include <db/storage/replacement_policy.h>
#include <db/storage/storage_engine.h>
#include <db/storage/table_info.h>
#include <db/storage/table_stats.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using db::ColumnRef;
    using db::PlanNode;
    using db::PlanOp;
    using db::Schema;
    using db::SortKey;
    using db::Type;
    using db::Value;
    using db::opt::DpEntry;
    using db::opt::DpTable;
    using db::opt::JoinAlgorithm;
    using db::opt::NodeEstimate;
    using db::opt::OptimizedPlan;
    using db::opt::OptimizerOptions;
    using db::opt::RuleId;
    using db::opt::RuleSet;
    using db::storage::ColumnStats;
    using db::storage::StorageEngine;
    using db::storage::TableInfo;
    using db::storage::TableStats;

    ColumnRef col(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    PlanNode select(const std::string& condition, PlanNode child)
    {
        return PlanNode::select(db::parseCondition(condition), std::move(child));
    }

    PlanNode join(const std::string& condition, PlanNode left, PlanNode right)
    {
        return PlanNode::join(db::parseCondition(condition), std::move(left), std::move(right));
    }

    // A table of INT columns (and one VARCHAR(20) when `varcharFirst`).
    TableInfo fakeTable(const std::string& name, const std::vector<std::string>& columns, std::uint32_t pages,
                        std::uint64_t rows, bool varcharFirst = false)
    {
        std::vector<db::Column> list;
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            bool isVarchar = varcharFirst && i == 0;
            list.push_back({name, columns[i], isVarchar ? Type::Varchar : Type::Int, isVarchar ? 20 : 0});
        }
        return TableInfo{name, Schema(list), name + ".db", 0, pages, rows, std::nullopt};
    }

    // An analyzed §M4.5 table: width 40.96, one ndv per column.
    TableInfo specTable(const std::string& name, const std::vector<std::string>& columns, std::uint32_t pages,
                        std::uint64_t rows, const std::vector<std::uint64_t>& ndvs)
    {
        TableInfo info = fakeTable(name, columns, pages, rows);
        TableStats stats{rows, pages, 40.96, {}};
        for (std::uint64_t ndv : ndvs)
        {
            stats.columns.push_back(ColumnStats{ndv, Value::ofInt(1), Value::ofInt(100000), {}});
        }
        info.stats = stats;
        return info;
    }

    db::opt::StatsProvider fakeStats()
    {
        return [](const std::string& name) -> std::optional<TableInfo> {
            if (name == "Emp")
            {
                return fakeTable("Emp", {"name", "did", "age"}, 200, 10000, true);
            }
            if (name == "Dept")
            {
                return fakeTable("Dept", {"did", "budget"}, 1, 50);
            }
            if (name == "A")
            {
                return specTable("A", {"x"}, 100, 5000, {2500});
            }
            if (name == "Bp")
            {
                return specTable("Bp", {"x", "y"}, 10, 500, {500, 500});
            }
            if (name == "C")
            {
                return specTable("C", {"y"}, 400, 20000, {5000});
            }
            if (name == "Q")
            {
                // 12 pages: a sort at B = 4 needs one more pass in exec
                // than the plain formula says (see the sort test).
                return fakeTable("Q", {"a", "b"}, 12, 4000);
            }
            if (name == "G")
            {
                // 100 pages, ndv(k) = 2,048: a group_by on k with count(*)
                // gives 2,048 rows of 8 bytes, exactly 4 pages.
                return specTable("G", {"k"}, 100, 10000, {2048});
            }
            if (name.size() == 2 && name[0] == 'T' && name[1] >= '1' && name[1] <= '7')
            {
                return fakeTable(name, {"k"}, 10, 1000);
            }
            if (name == "R")
            {
                return fakeTable("R", {"a", "b"}, 100, 10000);
            }
            if (name == "S")
            {
                return fakeTable("S", {"b", "c"}, 100, 10000);
            }
            return std::nullopt;
        };
    }

    OptimizerOptions withFrames(std::int64_t frames)
    {
        OptimizerOptions options;
        options.frames = frames;
        return options;
    }

    OptimizedPlan optimizeFake(const PlanNode& logical, const OptimizerOptions& options)
    {
        return db::opt::optimize(logical, fakeStats(), options);
    }

    std::string json(const PlanNode& plan)
    {
        return db::toJsonText(plan);
    }

    PlanNode seqscan(const std::string& table)
    {
        PlanNode node;
        node.op = PlanOp::SeqScan;
        node.table = table;
        return node;
    }

    // A physical filter or join node.
    PlanNode physical(PlanOp op, const std::string& condition, std::vector<PlanNode> children)
    {
        PlanNode node;
        node.op = op;
        node.condition = db::parseCondition(condition);
        node.children = std::move(children);
        return node;
    }

    // The tables of a plan's scans, in pre-order. For a left-deep join tree
    // this is the join order: the first outer, then each inner.
    void collectTables(const PlanNode& plan, std::vector<std::string>& out)
    {
        if (plan.op == PlanOp::SeqScan || plan.op == PlanOp::Scan)
        {
            out.push_back(plan.table);
        }
        for (const PlanNode& child : plan.children)
        {
            collectTables(child, out);
        }
    }

    std::vector<std::string> tablesInOrder(const PlanNode& plan)
    {
        std::vector<std::string> out;
        collectTables(plan, out);
        return out;
    }

    bool contains(const PlanNode& plan, PlanOp op)
    {
        if (plan.op == op)
        {
            return true;
        }
        for (const PlanNode& child : plan.children)
        {
            if (contains(child, op))
            {
                return true;
            }
        }
        return false;
    }

    // The first node with op `op` in pre-order (the plan must have one).
    const PlanNode& firstNode(const PlanNode& plan, PlanOp op)
    {
        const PlanNode* node = &plan;
        while (node->op != op)
        {
            REQUIRE(!node->children.empty());
            node = &node->children.front();
        }
        return *node;
    }

    // The DP entry of the group made of the relations named `names`.
    const DpEntry& entryOf(const DpTable& table, const std::vector<std::string>& names)
    {
        std::uint32_t mask = 0;
        for (const std::string& name : names)
        {
            bool found = false;
            for (std::size_t i = 0; i < table.names.size(); ++i)
            {
                if (table.names[i] == name)
                {
                    mask |= std::uint32_t{1} << i;
                    found = true;
                }
            }
            REQUIRE(found);
        }
        for (const DpEntry& entry : table.entries)
        {
            if (entry.subset == mask)
            {
                return entry;
            }
        }
        FAIL("no entry for that group");
        return table.entries.front();
    }

    // The §M4.3 query with commas: Emp e, Dept d WHERE e.did = d.did AND
    // e.age > 60, the shape sql builds (project / select / cross).
    PlanNode empDeptComma()
    {
        return PlanNode::project(
            {col("e", "name")},
            select("e.did = d.did and e.age > 60",
                   PlanNode::cross(PlanNode::rename("e", PlanNode::scan("Emp")),
                                   PlanNode::rename("d", PlanNode::scan("Dept")))));
    }

    // The same query with JOIN ON (project / select / join).
    PlanNode empDeptJoinOn()
    {
        return PlanNode::project(
            {col("e", "name")},
            select("e.age > 60", join("e.did = d.did", PlanNode::rename("e", PlanNode::scan("Emp")),
                                      PlanNode::rename("d", PlanNode::scan("Dept")))));
    }

    // §M4.5's three relations written in the order A, C, B', so the DP has
    // to move B' to the front.
    PlanNode specQueryACB()
    {
        return select("A.x = Bp.x and Bp.y = C.y",
                      PlanNode::cross(PlanNode::cross(PlanNode::scan("A"), PlanNode::scan("C")),
                                      PlanNode::scan("Bp")));
    }
} // namespace

TEST_CASE("Comma join and JOIN ON give the same physical plan (§M4.9)")
{
    for (bool unoptimized : {false, true})
    {
        CAPTURE(unoptimized);
        OptimizerOptions options = withFrames(32);
        options.unoptimized = unoptimized;

        OptimizedPlan comma = optimizeFake(empDeptComma(), options);
        OptimizedPlan joinOn = optimizeFake(empDeptJoinOn(), options);

        CHECK(json(comma.physical) == json(joinOn.physical));
        CHECK(comma.estimates == joinOn.estimates);
        // The input is kept as it came; the rewritten trees agree.
        CHECK(db::toString(comma.logicalBefore) == db::toString(empDeptComma()));
        CHECK(db::toString(comma.logicalAfter) == db::toString(joinOn.logicalAfter));
    }
}

TEST_CASE("The DP decides the join order: §M4.5 written as A, C, B' (B = 32)")
{
    OptimizedPlan result = optimizeFake(specQueryACB(), withFrames(32));

    // One block of three relations, numbered in written order.
    REQUIRE(result.dpTables.size() == 1);
    const DpTable& table = result.dpTables[0];
    CHECK(table.names == std::vector<std::string>{"A", "C", "Bp"});

    // The golden table: the same costs as §M4.5, whatever the written order.
    CHECK(entryOf(table, {"A"}).cost == 100);
    CHECK(entryOf(table, {"Bp"}).cost == 10);
    CHECK(entryOf(table, {"C"}).cost == 400);
    CHECK(entryOf(table, {"A", "Bp"}).cost == 110);
    CHECK(entryOf(table, {"Bp", "C"}).cost == 410);
    CHECK(entryOf(table, {"A", "C"}).skipped);
    CHECK(entryOf(table, {"A", "Bp", "C"}).cost == 530);
    // The sizes the planner worked out from the statistics: §M4.5's
    // 1,000 rows on 20 pages and 2,000 rows on 40 pages.
    CHECK(entryOf(table, {"A", "Bp"}).rows == 1000);
    CHECK(entryOf(table, {"A", "Bp"}).pages == 20);
    CHECK(entryOf(table, {"Bp", "C"}).rows == 2000);
    CHECK(entryOf(table, {"Bp", "C"}).pages == 40);

    // The chosen order: B' first, then A, then C, both nested loops.
    CHECK(table.first == 2);
    REQUIRE(table.steps.size() == 2);
    CHECK(table.steps[0].inner == 0);
    CHECK(table.steps[0].algorithm == JoinAlgorithm::NestedLoop);
    CHECK(table.steps[1].inner == 1);
    CHECK(table.steps[1].algorithm == JoinAlgorithm::NestedLoop);

    // The physical plan has that shape, with the same algorithms the DP
    // chose (STATUS 2026-11-12 note: chunk 10 chooses again from the
    // rebuilt tree, and here it agrees), and a project on top that puts
    // the columns back in the written order A, C, B'.
    PlanNode expected = PlanNode::project(
        {col("A", "x"), col("C", "y"), col("Bp", "x"), col("Bp", "y")},
        physical(PlanOp::NestedLoopJoin, "Bp.y = C.y",
                 {physical(PlanOp::NestedLoopJoin, "A.x = Bp.x", {seqscan("Bp"), seqscan("A")}), seqscan("C")}));
    CHECK(json(result.physical) == json(expected));
}

TEST_CASE("est_cost: each node's own cost with the frames exec gives it (decision 5)")
{
    // The plan of the previous test at B = 32, in pre-order: project,
    // upper join, lower join, scan Bp, scan A, scan C. Both joins run at the
    // same time, so exec's divideFrames gives the lower join only its
    // minimum of 3 frames (a block of 1 page) and the upper one a block of
    // 32 - 3 - 1 = 28 pages.
    // - upper: 20 + ceil(20 / 28) x 400 = 420 (the DP table says 420 too);
    // - lower: 10 + ceil(10 / 1) x 100 = 1,010 (the DP table says 110: it
    //   gives every join the full B, §M4.5);
    // - each scan its pages; project 0.
    OptimizedPlan result = optimizeFake(specQueryACB(), withFrames(32));

    REQUIRE(result.estimates.size() == 6);
    std::vector<std::int64_t> costs;
    for (const NodeEstimate& estimate : result.estimates)
    {
        costs.push_back(estimate.cost);
    }
    CHECK(costs == std::vector<std::int64_t>{0, 420, 1010, 10, 100, 400});
    // Rows as estimated: the lower join 1,000, the upper 1,000 x 20,000 /
    // 5,000 = 4,000.
    CHECK(result.estimates[1].rows == 4000);
    CHECK(result.estimates[2].rows == 1000);
}

TEST_CASE("est_cost of a sort counts passes the way exec runs them (Q2 (b))")
{
    // Q has 12 pages. At B = 4 the plain §M4.4 formula gives 2 passes:
    // 2 x 12 x 1 + 12 = 36. Exec's sort at the root makes its runs with
    // B - 1 = 3 frames (the scan below holds one): 4 runs; merges 3 at a
    // time into 2 runs (the leftover run is copied); the final merge
    // streams them: 3 passes, 2 x 12 x 2 + 12 = 60.
    PlanNode logical = PlanNode::sort({SortKey{col("Q", "a"), true}}, PlanNode::scan("Q"));

    OptimizedPlan result = optimizeFake(logical, withFrames(4));

    REQUIRE(result.estimates.size() == 2);
    CHECK(result.estimates[0].cost == 60);
    CHECK(result.estimates[1].cost == 12);
}

TEST_CASE("est_cost counts an input that exec writes to a run (B = 4)")
{
    // T1 x T2 x T3 in written order (unoptimized): two nested loops. At
    // B = 4 the upper join's outer (the lower join, which streams with 3
    // frames) does not fit beside a block, so exec writes it to a run once
    // and joins with a block of 2 (exec INTERFACE.md, writeOuter).
    // Lower join: 1,000 x 1,000 rows of 81.92 bytes = 20,000 pages.
    // Upper join: 20,000 + ceil(20,000 / 2) x 10 = 120,000, plus the
    // 20,000 pages written: 140,000.
    OptimizerOptions options = withFrames(4);
    options.unoptimized = true;
    PlanNode crosses = PlanNode::cross(PlanNode::cross(PlanNode::scan("T1"), PlanNode::scan("T2")),
                                       PlanNode::scan("T3"));

    OptimizedPlan result = optimizeFake(crosses, options);

    REQUIRE(result.estimates.size() == 5);
    CHECK(result.estimates[1].pages == 20000);
    CHECK(result.estimates[0].cost == 140000);
}

TEST_CASE("est_cost of a hash group_by: the table gets the node's own frames (B = 4)")
{
    // group_by G.k with count(*): 2,048 groups on 4 pages. At B = 4 the
    // table gets 3 frames (the scan holds the 4th), so the groups do not
    // fit: 3 x 100 + 2 x 3 = 306 (DECISIONS 2026-11-07, Hashing). Hash
    // still beats sort (700), so the mode is hash.
    PlanNode logical = PlanNode::groupBy({col("G", "k")},
                                         {db::Aggregate{db::AggregateFunction::Count, std::nullopt, "n"}},
                                         PlanNode::scan("G"));

    OptimizedPlan result = optimizeFake(logical, withFrames(4));

    REQUIRE(result.physical.op == PlanOp::GroupBy);
    CHECK(result.physical.mode == db::ExecMode::Hash);
    CHECK(result.estimates[0].pages == 4);
    CHECK(result.estimates[0].cost == 306);
}

TEST_CASE("The DP may choose hash joins, and the plan follows (§M4.5 at B = 4)")
{
    // At B = 4 hash wins both joins (chunk 11's B = 4 numbers: 330, then
    // 1,590). The DP must know which predicates are equalities to try it.
    OptimizedPlan result = optimizeFake(specQueryACB(), withFrames(4));

    REQUIRE(result.dpTables.size() == 1);
    const DpTable& table = result.dpTables[0];
    REQUIRE(table.steps.size() == 2);
    CHECK(table.steps[0].algorithm == JoinAlgorithm::Hash);
    CHECK(table.steps[1].algorithm == JoinAlgorithm::Hash);
    CHECK(entryOf(table, {"A", "Bp", "C"}).cost == 1590);
    // Chunk 10 chooses the same two hash joins for the rebuilt tree.
    CHECK(tablesInOrder(result.physical) == std::vector<std::string>{"Bp", "A", "C"});
    CHECK(firstNode(result.physical, PlanOp::HashJoin).children[0].op == PlanOp::HashJoin);
    CHECK(!contains(result.physical, PlanOp::NestedLoopJoin));
}

TEST_CASE("Rule 6 runs again after the DP: a project between the reordered joins")
{
    // SELECT C.y over A, C, B'. After the DP's order (B' A) C, the upper
    // join needs only Bp.y from the lower one (its condition), so rule 6
    // puts project[Bp.y] between the two joins. The written order had no
    // such place, so only the second rule 6 run can add it.
    PlanNode logical = PlanNode::project({col("C", "y")}, specQueryACB());

    OptimizedPlan result = optimizeFake(logical, withFrames(32));

    PlanNode expected = PlanNode::project(
        {col("C", "y")},
        physical(PlanOp::NestedLoopJoin, "Bp.y = C.y",
                 {PlanNode::project({col("Bp", "y")}, physical(PlanOp::NestedLoopJoin, "A.x = Bp.x",
                                                               {seqscan("Bp"), seqscan("A")})),
                  seqscan("C")}));
    CHECK(json(result.physical) == json(expected));
}

TEST_CASE("Unoptimized mode: written order, one fixed algorithm, no pushdown (§M4.7 #2)")
{
    OptimizerOptions options = withFrames(32);
    options.unoptimized = true;

    SUBCASE("Emp and Dept: the select stays above the join, no projections")
    {
        // Rule 3 still makes the equality a join condition; rule 2 is off,
        // so e.age > 60 stays above the join; rule 6 is off, so no
        // projection is pushed down.
        OptimizedPlan result = optimizeFake(empDeptComma(), options);
        PlanNode expected = PlanNode::project(
            {col("e", "name")},
            physical(PlanOp::Filter, "e.age > 60",
                     {physical(PlanOp::NestedLoopJoin, "e.did = d.did",
                               {PlanNode::rename("e", seqscan("Emp")), PlanNode::rename("d", seqscan("Dept"))})}));
        CHECK(json(result.physical) == json(expected));
        CHECK(result.dpTables.empty());

        // Another fixed algorithm is used where it can run.
        options.fixedAlgorithm = JoinAlgorithm::Hash;
        CHECK(firstNode(optimizeFake(empDeptComma(), options).physical, PlanOp::Filter).children[0].op ==
              PlanOp::HashJoin);
    }

    SUBCASE("A, C, B': the written order stays, A x C stays a cross")
    {
        OptimizedPlan result = optimizeFake(specQueryACB(), options);
        CHECK(tablesInOrder(result.physical) == std::vector<std::string>{"A", "C", "Bp"});
        // Both predicates name B', so they sit on the upper join; the lower
        // join is the cross A x C: a nested loop on 1 = 1.
        const PlanNode& upper = firstNode(result.physical, PlanOp::NestedLoopJoin);
        const PlanNode& lower = firstNode(upper.children[0], PlanOp::NestedLoopJoin);
        REQUIRE(lower.condition.has_value());
        CHECK(db::toString(*lower.condition) == "1 = 1");
        CHECK(!contains(result.physical, PlanOp::HashJoin));
        CHECK(!contains(result.physical, PlanOp::SortMergeJoin));
        CHECK(result.dpTables.empty());
    }
}

TEST_CASE("Rule switches: with rule 2 off, every select stays above the join (§M4.7 #3)")
{
    OptimizerOptions options = withFrames(32);
    options.rules[static_cast<std::size_t>(RuleId::PushSelectBelowJoin) - 1] = false;

    OptimizedPlan result = optimizeFake(empDeptComma(), options);

    const PlanNode& joinNode = firstNode(result.logicalAfter, PlanOp::Join);
    CHECK(contains(result.logicalAfter, PlanOp::Select));
    CHECK(!contains(joinNode.children[0], PlanOp::Select));
    CHECK(!contains(joinNode.children[1], PlanOp::Select));
    // The join ordering keeps it there too: e.age > 60 uses one relation,
    // so it is no join predicate, and it stays as a filter above the join.
    REQUIRE(contains(result.physical, PlanOp::Filter));
    const PlanNode& filter = firstNode(result.physical, PlanOp::Filter);
    CHECK(db::toString(*filter.condition) == "e.age > 60");
    CHECK(filter.children[0].op != PlanOp::Filter);
    CHECK(!contains(filter.children[0], PlanOp::Filter));

    // With every rule on, e.age > 60 moves below the join.
    OptimizedPlan normal = optimizeFake(empDeptComma(), withFrames(32));
    const PlanNode& normalJoin = firstNode(normal.logicalAfter, PlanOp::Join);
    CHECK(contains(normalJoin.children[0], PlanOp::Select));
}

TEST_CASE("Column order: reordered joins keep the written column order")
{
    // SELECT * over A, C, B': the DP joins B', A, C, whose output would be
    // Bp.x, Bp.y, A.x, C.y. The top project restores A.x, C.y, Bp.x, Bp.y.
    OptimizedPlan result = optimizeFake(specQueryACB(), withFrames(32));

    REQUIRE(result.physical.op == PlanOp::Project);
    CHECK(result.physical.columns ==
          std::vector<ColumnRef>{col("A", "x"), col("C", "y"), col("Bp", "x"), col("Bp", "y")});

    // When nothing moved, no project is added: §M4.5 written as B', A, C.
    PlanNode inOrder = select("A.x = Bp.x and Bp.y = C.y",
                              PlanNode::cross(PlanNode::cross(PlanNode::scan("Bp"), PlanNode::scan("A")),
                                              PlanNode::scan("C")));
    CHECK(optimizeFake(inOrder, withFrames(32)).physical.op == PlanOp::NestedLoopJoin);
}

TEST_CASE("More than six relations: the written order is kept, and the DP table says so")
{
    // T1.k = T2.k and T2.k = T3.k and ... and T6.k = T7.k over a chain of
    // crosses in written order.
    PlanNode crosses = PlanNode::scan("T1");
    std::string condition;
    for (int i = 2; i <= 7; ++i)
    {
        std::string table = "T" + std::to_string(i);
        crosses = PlanNode::cross(std::move(crosses), PlanNode::scan(table));
        if (!condition.empty())
        {
            condition += " and ";
        }
        condition += "T" + std::to_string(i - 1) + ".k = " + table + ".k";
    }

    OptimizedPlan result = optimizeFake(select(condition, std::move(crosses)), withFrames(32));

    CHECK(tablesInOrder(result.physical) ==
          std::vector<std::string>{"T1", "T2", "T3", "T4", "T5", "T6", "T7"});
    REQUIRE(result.dpTables.size() == 1);
    CHECK(result.dpTables[0].names.size() == 7);
    CHECK(result.dpTables[0].entries.empty());
    // A valid plan: exec's physical reader gives the same tree back.
    CHECK(json(db::physicalFromJsonText(json(result.physical))) == json(result.physical));
}

TEST_CASE("Several join blocks are planned on their own")
{
    PlanNode rs = PlanNode::project({col("R", "a")},
                                    select("R.b = S.b", PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("S"))));

    SUBCASE("a union of two join queries: two DP tables, left first")
    {
        PlanNode other = PlanNode::project(
            {col("R", "a")}, select("R.a = S.c", PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("S"))));
        OptimizedPlan result = optimizeFake(PlanNode::unionOf(rs, other), withFrames(32));
        REQUIRE(result.dpTables.size() == 2);
        CHECK(result.dpTables[0].names == std::vector<std::string>{"R", "S"});
        CHECK(result.dpTables[1].names == std::vector<std::string>{"R", "S"});
        REQUIRE(result.dpTables[0].steps.size() == 1);
        REQUIRE(result.dpTables[1].steps.size() == 1);
        CHECK(db::toString(db::conjoin(result.dpTables[0].steps[0].conditions)) == "R.b = S.b");
        CHECK(db::toString(db::conjoin(result.dpTables[1].steps[0].conditions)) == "R.a = S.c");
    }

    SUBCASE("a derived table with its own join: inner block first")
    {
        PlanNode outer = join("t.a = Dept.did", PlanNode::rename("t", rs), PlanNode::scan("Dept"));
        OptimizedPlan result = optimizeFake(outer, withFrames(32));
        REQUIRE(result.dpTables.size() == 2);
        CHECK(result.dpTables[0].names == std::vector<std::string>{"R", "S"});
        CHECK(result.dpTables[1].names == std::vector<std::string>{"t", "Dept"});
    }
}

TEST_CASE("Deep trees: 998 selects above a join (kMaxNestingDepth)")
{
    PlanNode logical = join("R.b = S.b", PlanNode::scan("R"), PlanNode::scan("S"));
    for (int i = 0; i < 998; ++i)
    {
        logical = select("R.a > " + std::to_string(i), std::move(logical));
    }

    OptimizedPlan result = optimizeFake(logical, withFrames(32));

    CHECK(tablesInOrder(result.physical).size() == 2);
    CHECK(result.dpTables.size() == 1);
}

TEST_CASE("An unknown table fails before any work")
{
    try
    {
        optimizeFake(PlanNode::scan("Nope"), withFrames(32));
        FAIL("expected an error");
    }
    catch (const db::Error& error)
    {
        CHECK(error.category() == db::ErrorCategory::Name);
    }
}

// ---------------------------------------------------------------------------
// Runs in exec (§M4.6, §M4.9): no statistics, optimized = unoptimized.

namespace
{
    // R(a, b): 200 rows, a = i, b = i mod 50. S(b, c): 50 rows, b = j,
    // c = 10 x j. W(c, d): 10 rows, c = 10 x k, d = k. Q(a, b): 4,000 rows,
    // a = (i x 37) mod 4,000 (shuffled), b = i, on 12 data pages.
    std::unique_ptr<StorageEngine> smallDatabase(const std::string& name)
    {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opt_planner_" + name);
        std::filesystem::remove_all(dir);
        std::unique_ptr<StorageEngine> engine = StorageEngine::open(dir, 32, db::storage::ReplacementPolicy::Lru);
        struct Table
        {
            std::string name;
            std::string first;
            std::string second;
            std::vector<std::pair<std::int32_t, std::int32_t>> rows;
        };
        std::vector<Table> tables(4);
        tables[0] = {"R", "a", "b", {}};
        for (std::int32_t i = 0; i < 200; ++i)
        {
            tables[0].rows.push_back({i, i % 50});
        }
        tables[1] = {"S", "b", "c", {}};
        for (std::int32_t j = 0; j < 50; ++j)
        {
            tables[1].rows.push_back({j, 10 * j});
        }
        tables[2] = {"W", "c", "d", {}};
        for (std::int32_t k = 0; k < 10; ++k)
        {
            tables[2].rows.push_back({10 * k, k});
        }
        tables[3] = {"Q", "a", "b", {}};
        for (std::int32_t i = 0; i < 4000; ++i)
        {
            tables[3].rows.push_back({(i * 37) % 4000, i});
        }
        for (const Table& table : tables)
        {
            engine->catalog().createTable(table.name, Schema({{table.name, table.first, Type::Int, 0},
                                                              {table.name, table.second, Type::Int, 0}}));
            db::storage::HeapFile heap = engine->openHeapFile(table.name);
            for (const auto& [first, second] : table.rows)
            {
                heap.insert({Value::ofInt(first), Value::ofInt(second)});
            }
            engine->catalog().updateCounts(table.name, heap.numDataPages(), table.rows.size());
        }
        return engine;
    }

    db::opt::StatsProvider catalogStats(StorageEngine& engine)
    {
        return [&engine](const std::string& name) { return engine.catalog().getTable(name); };
    }

    // Optimizes `logical` with the catalog's statistics, runs the physical
    // plan in exec at B = `frames`, and returns every result row.
    std::multiset<db::Tuple> runRows(StorageEngine& engine, const PlanNode& logical, std::int64_t frames,
                                     bool unoptimized)
    {
        OptimizerOptions options;
        options.frames = frames;
        options.unoptimized = unoptimized;
        PlanNode plan = db::opt::optimize(logical, catalogStats(engine), options).physical;
        engine.resetPool(static_cast<std::size_t>(frames));
        db::exec::ExecContext ctx(engine);
        db::exec::ExecutablePlan executable = db::exec::buildExecutablePlan(plan, ctx);
        executable.open();
        std::multiset<db::Tuple> rows;
        db::Tuple tuple;
        while (executable.next(tuple))
        {
            rows.insert(tuple);
        }
        executable.close();
        return rows;
    }
} // namespace

TEST_CASE("Optimized and unoptimized plans give the same rows in exec, with and without statistics")
{
    std::unique_ptr<StorageEngine> engine = smallDatabase("same_rows");

    // R and S, written with commas and with JOIN ON: a > 100 keeps 99 rows,
    // each with exactly one S row.
    PlanNode comma = PlanNode::project(
        {col("R", "a"), col("S", "c")},
        select("R.b = S.b and R.a > 100", PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("S"))));
    PlanNode joinOn = PlanNode::project({col("R", "a"), col("S", "c")},
                                        select("R.a > 100", join("R.b = S.b", PlanNode::scan("R"), PlanNode::scan("S"))));
    // R, W, S written with no predicate between R and W, SELECT *: R rows
    // with b < 10 (40 of them) match S.b = b and W.c = 10 x b.
    PlanNode threeWay = select("R.b = S.b and S.c = W.c",
                               PlanNode::cross(PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("W")),
                                               PlanNode::scan("S")));

    // First with no statistics (documented defaults, §M4.9), then analyzed.
    for (bool analyzed : {false, true})
    {
        CAPTURE(analyzed);
        if (analyzed)
        {
            for (const char* table : {"R", "S", "W"})
            {
                db::opt::analyzeTable(*engine, table, 20);
            }
        }
        for (std::int64_t frames : {4, 32})
        {
            CAPTURE(frames);
            std::multiset<db::Tuple> expected = runRows(*engine, comma, frames, true);
            CHECK(expected.size() == 99);
            CHECK(runRows(*engine, comma, frames, false) == expected);
            CHECK(runRows(*engine, joinOn, frames, false) == expected);
            CHECK(runRows(*engine, joinOn, frames, true) == expected);

            // Same rows in the same column order (R, W, S) after the DP
            // moves the relations.
            std::multiset<db::Tuple> threeExpected = runRows(*engine, threeWay, frames, true);
            CHECK(threeExpected.size() == 40);
            CHECK(runRows(*engine, threeWay, frames, false) == threeExpected);
        }
    }
}

TEST_CASE("est_cost of a sort equals exec's measured I/O (12 pages, B = 4)")
{
    std::unique_ptr<StorageEngine> engine = smallDatabase("sort_cost");
    REQUIRE(engine->catalog().getTable("Q")->numPages == 12);

    PlanNode logical = PlanNode::sort({SortKey{col("Q", "a"), true}}, PlanNode::scan("Q"));
    OptimizerOptions options;
    options.frames = 4;
    OptimizedPlan result = db::opt::optimize(logical, catalogStats(*engine), options);

    engine->resetPool(4);
    db::exec::ExecContext ctx(*engine);
    db::exec::ExecutablePlan executable = db::exec::buildExecutablePlan(result.physical, ctx);
    executable.open();
    db::Tuple tuple;
    while (executable.next(tuple))
    {
    }
    executable.close();

    // The sort's subtree I/O includes its input scan, which also reads one
    // directory page that the cost model leaves out (decision 2026-11-09).
    const db::exec::ExecCounters& measured = executable.trace()[0].subtree;
    CHECK(result.estimates[0].cost == 60);
    CHECK(static_cast<std::int64_t>(measured.pageReads + measured.pageWrites) - 1 == result.estimates[0].cost);
}

TEST_CASE("est_cost of two-input sorts equals exec's measured I/O (sort-merge, intersect, except)")
{
    // Both inputs are Q (12 pages), every key matches, so each input is read
    // to the end. Exec gives the two final merges stream - 1 frames
    // (sort-merge; one holds the run of equal keys) or stream frames
    // (intersect, except), the left sort the larger half: at B = 4 a
    // sort-merge join's left sort reads 2 runs at once, the right only 1.
    // The odd and even B below catch a split that is even, or favours the
    // right side.
    std::unique_ptr<StorageEngine> engine = smallDatabase("two_sorts");
    PlanNode copy = PlanNode::rename("q", PlanNode::scan("Q"));
    std::vector<PlanNode> plans = {join("Q.a = q.a", PlanNode::scan("Q"), copy),
                                   PlanNode::intersect(PlanNode::scan("Q"), copy),
                                   PlanNode::except(PlanNode::scan("Q"), copy)};
    for (const PlanNode& logical : plans)
    {
        for (std::int64_t frames : {3, 4, 5, 6, 8})
        {
            CAPTURE(frames);
            OptimizerOptions options;
            options.frames = frames;
            options.unoptimized = true;
            options.fixedAlgorithm = JoinAlgorithm::SortMerge;
            OptimizedPlan result = db::opt::optimize(logical, catalogStats(*engine), options);
            engine->resetPool(static_cast<std::size_t>(frames));
            db::exec::ExecContext ctx(*engine);
            db::exec::ExecutablePlan executable = db::exec::buildExecutablePlan(result.physical, ctx);
            executable.open();
            db::Tuple tuple;
            while (executable.next(tuple))
            {
            }
            executable.close();
            // The root's subtree includes both input scans, and each reads
            // one directory page the cost model leaves out.
            const db::exec::ExecCounters& measured = executable.trace()[0].subtree;
            CAPTURE(executable.trace()[0].label);
            CHECK(static_cast<std::int64_t>(measured.pageReads + measured.pageWrites) - 2 ==
                  result.estimates[0].cost);
        }
    }
}
