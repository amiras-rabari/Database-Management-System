// Chunk 10: logical plan -> physical plan in exec's JSON format (§M4.6),
// the join algorithm and mode choices, and aggregate names.
//
// R(a, b) and S(b, c) are made-up tables of 10,000 rows on 100 pages each,
// never analyzed, so every estimate uses the documented defaults; T is the
// same size but analyzed. The "runs
// unedited" tests build a small real database and run the plans in exec.

#include <doctest/doctest.h>

#include <db/common/condition.h>
#include <db/common/plan.h>
#include <db/common/plan_json.h>
#include <db/common/schema.h>
#include <db/common/value.h>
#include <db/exec/exec_context.h>
#include <db/exec/executable_plan.h>
#include <db/opt/estimate.h>
#include <db/opt/physical.h>
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
#include <string>
#include <utility>
#include <vector>

namespace
{
    using db::Aggregate;
    using db::AggregateFunction;
    using db::ColumnRef;
    using db::ExecMode;
    using db::PlanNode;
    using db::PlanOp;
    using db::Schema;
    using db::SortKey;
    using db::Type;
    using db::Value;
    using db::opt::JoinAlgorithm;
    using db::opt::PhysicalResult;
    using db::storage::ColumnStats;
    using db::storage::StorageEngine;
    using db::storage::TableInfo;
    using db::storage::TableStats;

    ColumnRef col(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }

    ColumnRef bare(const std::string& name)
    {
        return ColumnRef{std::nullopt, name};
    }

    PlanNode select(const std::string& condition, PlanNode child)
    {
        return PlanNode::select(db::parseCondition(condition), std::move(child));
    }

    PlanNode join(const std::string& condition, PlanNode left, PlanNode right)
    {
        return PlanNode::join(db::parseCondition(condition), std::move(left), std::move(right));
    }

    // A table of two INT columns, not analyzed: R(a, b) and S(b, c) have
    // 10,000 rows on 100 pages, U(a, b) 100,000 rows on 1,000 pages.
    TableInfo fakeTable(const std::string& name, const std::string& first, const std::string& second,
                        std::uint32_t pages = 100, std::uint64_t rows = 10000)
    {
        Schema schema({{name, first, Type::Int, 0}, {name, second, Type::Int, 0}});
        return TableInfo{name, schema, name + ".db", 0, pages, rows, std::nullopt};
    }

    db::opt::StatsProvider fakeStats()
    {
        return [](const std::string& name) -> std::optional<TableInfo> {
            if (name == "R")
            {
                return fakeTable("R", "a", "b");
            }
            if (name == "S")
            {
                return fakeTable("S", "b", "c");
            }
            if (name == "U")
            {
                return fakeTable("U", "a", "b", 1000, 100000);
            }
            if (name == "T")
            {
                // Like R, but analyzed: ndv(a) = 100, ndv(b) = 10,000.
                TableInfo info = fakeTable("T", "a", "b");
                info.stats = TableStats{10000, 100, 40.96,
                                        {ColumnStats{100, Value::ofInt(1), Value::ofInt(100), {}},
                                         ColumnStats{10000, Value::ofInt(1), Value::ofInt(10000), {}}}};
                return info;
            }
            return std::nullopt;
        };
    }

    PhysicalResult convert(const PlanNode& logical, std::int64_t frames,
                           std::optional<JoinAlgorithm> fixed = std::nullopt)
    {
        return db::opt::toPhysical(logical, fakeStats(), frames, fixed);
    }

    // Plans are compared as JSON text: physical plans have no one-line
    // toString, and the text shows what differs when a check fails.
    std::string json(const PlanNode& plan)
    {
        return db::toJsonText(plan);
    }

    // A physical seqscan of `table`.
    PlanNode seqscan(const std::string& table)
    {
        PlanNode node;
        node.op = PlanOp::SeqScan;
        node.table = table;
        return node;
    }

    // A physical filter or join node with its condition and children.
    PlanNode physical(PlanOp op, const std::string& condition, std::vector<PlanNode> children)
    {
        PlanNode node;
        node.op = op;
        node.condition = db::parseCondition(condition);
        node.children = std::move(children);
        return node;
    }

    // The logical group_by of the aggregate tests: per R.b, count(*) as n
    // and avg(R.a) as avg_a.
    PlanNode groupByB()
    {
        return PlanNode::groupBy({col("R", "b")},
                                 {Aggregate{AggregateFunction::Count, std::nullopt, "n"},
                                  Aggregate{AggregateFunction::Avg, col("R", "a"), "avg_a"}},
                                 PlanNode::scan("R"));
    }
} // namespace

TEST_CASE("Name mapping: scan -> seqscan, select -> filter, estimates in pre-order")
{
    PlanNode logical = PlanNode::project({col("R", "a")}, select("R.a > 500", PlanNode::scan("R")));

    PhysicalResult result = convert(logical, 32);

    PlanNode expected = PlanNode::project(
        {col("R", "a")}, physical(PlanOp::Filter, "R.a > 500", {seqscan("R")}));
    CHECK(json(result.plan) == json(expected));
    // Same nodes in the same order, so the estimates are the logical ones:
    // root first (project), then the filter, then the scan.
    REQUIRE(result.estimates.size() == 3);
    CHECK(result.estimates == db::opt::estimatePlan(logical, fakeStats()));
    CHECK(result.estimates[2].pages == 100);
    CHECK(result.estimates[1].rows == 3334); // 10,000 x 1/3 (default range)
}

TEST_CASE("B changes the join algorithm (R and S 100 pages each, R.b = S.b)")
{
    PlanNode logical = join("R.b = S.b", PlanNode::scan("R"), PlanNode::scan("S"));

    // B = 102: nested loop 100 + 1 x 100 = 200, sort-merge 100 + 100 = 200
    // (each input sorts in one pass), hash 600. The tie goes to the first
    // in JoinAlgorithm order: nested loop.
    CHECK(convert(logical, 102).plan.op == PlanOp::NestedLoopJoin);
    // B = 4: nested loop 100 + 50 x 100 = 5,100, sort-merge 700 + 700 =
    // 1,400, hash 600.
    PhysicalResult small = convert(logical, 4);
    CHECK(json(small.plan) == json(physical(PlanOp::HashJoin, "R.b = S.b", {seqscan("R"), seqscan("S")})));
}

TEST_CASE("Each input's own page count goes into the formulas (R 100 pages, U 1,000 pages)")
{
    // B = 27: nested loop 100 + ceil(100/25) x 1,000 = 4,100; sort-merge
    // 300 + 5,000 = 5,300; hash 3 x 1,100 = 3,300. Hash wins. Reading U as
    // 100 pages would give nested loop 500 against hash 600.
    PlanNode logical = join("R.b = U.b", PlanNode::scan("R"), PlanNode::scan("U"));
    CHECK(convert(logical, 27).plan.op == PlanOp::HashJoin);
}

TEST_CASE("Keys of different types: a REAL avg joined to an INT column stays a nested loop")
{
    // g.avg_a is AVG's REAL output, S.b an INT. They compare as numbers, so
    // a nested loop runs it, but exec's sort-merge and hash joins need one
    // type. The alias becomes the generated name in the join condition too.
    PlanNode derived = PlanNode::rename(
        "g", PlanNode::groupBy({col("R", "b")}, {Aggregate{AggregateFunction::Avg, col("R", "a"), "avg_a"}},
                               PlanNode::scan("R")));
    PlanNode logical = join("g.avg_a = S.b", derived, PlanNode::scan("S"));

    PhysicalResult result = convert(logical, 4);

    CHECK(result.plan.op == PlanOp::NestedLoopJoin);
    CHECK((*result.plan.condition == db::parseCondition("g.avg_R_a = S.b")));
    // Forcing hash falls back too.
    CHECK(convert(logical, 4, JoinAlgorithm::Hash).plan.op == PlanOp::NestedLoopJoin);
    // The same with the derived table as the right input: its aggregate
    // names must reach the join condition as well.
    PhysicalResult mirrored = convert(join("S.b = g.avg_a", PlanNode::scan("S"), derived), 4);
    CHECK((*mirrored.plan.condition == db::parseCondition("S.b = g.avg_R_a")));
}

TEST_CASE("A non-equality join is always a nested loop")
{
    PlanNode logical = join("R.a < S.c", PlanNode::scan("R"), PlanNode::scan("S"));
    for (std::int64_t frames : {3, 4, 32, 102, 1000})
    {
        CHECK(convert(logical, frames).plan.op == PlanOp::NestedLoopJoin);
    }
    // A literal on one side is not a column = column equality either.
    CHECK(convert(join("R.b = 7", PlanNode::scan("R"), PlanNode::scan("S")), 4).plan.op ==
          PlanOp::NestedLoopJoin);
}

TEST_CASE("Mixed condition: the equality stays on the join, the rest goes to a filter above it")
{
    PlanNode logical = join("R.b = S.b and R.a < S.c", PlanNode::scan("R"), PlanNode::scan("S"));

    PhysicalResult result = convert(logical, 4);

    PlanNode expected = physical(
        PlanOp::Filter, "R.a < S.c",
        {physical(PlanOp::HashJoin, "R.b = S.b", {seqscan("R"), seqscan("S")})});
    CHECK(json(result.plan) == json(expected));
    // One estimate per physical node: the join keeps 10,000 x 10,000 x 0.1
    // (no statistics: default equality), the filter 0.1 of that (any other
    // join predicate, §M4.2).
    REQUIRE(result.estimates.size() == 4);
    CHECK(result.estimates[0].rows == 1000000);
    CHECK(result.estimates[1].rows == 10000000);
    CHECK(result.estimates[2].pages == 100);
    CHECK(result.estimates[3].pages == 100);
}

TEST_CASE("Cross: a nested loop with the condition 1 = 1")
{
    PhysicalResult result = convert(PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("S")), 4);
    CHECK(json(result.plan) == json(physical(PlanOp::NestedLoopJoin, "1 = 1", {seqscan("R"), seqscan("S")})));
    // Even when hash is forced, a cross has no keys: nested loop.
    CHECK(convert(PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("S")), 4, JoinAlgorithm::Hash).plan.op ==
          PlanOp::NestedLoopJoin);
}

TEST_CASE("Aggregates: 2-element physical aggregates, aliases above become generated names")
{
    // sort[n desc](project[R.b, n, avg_a](select[n > 5](group_by)))
    PlanNode logical = PlanNode::sort(
        {SortKey{bare("n"), false}},
        PlanNode::project({col("R", "b"), bare("n"), bare("avg_a")}, select("n > 5", groupByB())));

    PhysicalResult result = convert(logical, 32);

    const PlanNode& sort = result.plan;
    REQUIRE(sort.op == PlanOp::Sort);
    CHECK(sort.sortKeys == std::vector<SortKey>{SortKey{bare("count_star"), false}});
    const PlanNode& project = sort.children.at(0);
    CHECK(project.columns == std::vector<ColumnRef>{col("R", "b"), bare("count_star"), bare("avg_R_a")});
    const PlanNode& filter = project.children.at(0);
    REQUIRE(filter.op == PlanOp::Filter);
    CHECK((*filter.condition == db::parseCondition("count_star > 5")));
    const PlanNode& groupBy = filter.children.at(0);
    REQUIRE(groupBy.op == PlanOp::GroupBy);
    CHECK(groupBy.mode.has_value());
    CHECK(groupBy.columns == std::vector<ColumnRef>{col("R", "b")});
    CHECK(groupBy.aggregates == std::vector<Aggregate>{Aggregate{AggregateFunction::Count, std::nullopt, ""},
                                                       Aggregate{AggregateFunction::Avg, col("R", "a"), ""}});
}

TEST_CASE("Aggregates through a rename: t.n becomes t.count_star")
{
    PlanNode logical = PlanNode::project(
        {col("t", "b"), col("t", "n")},
        PlanNode::rename("t", PlanNode::groupBy({col("R", "b")},
                                                {Aggregate{AggregateFunction::Count, std::nullopt, "n"}},
                                                PlanNode::scan("R"))));

    PhysicalResult result = convert(logical, 32);

    CHECK(result.plan.columns == std::vector<ColumnRef>{col("t", "b"), col("t", "count_star")});
    CHECK(result.plan.children.at(0).op == PlanOp::Rename);
    CHECK(result.plan.children.at(0).alias == "t");
}

TEST_CASE("JSON round trip: exec's physical reader gives back the same tree")
{
    PlanNode aggregates = PlanNode::project({col("R", "b"), bare("n")}, select("n > 5", groupByB()));
    PlanNode joins = PlanNode::limit(
        10, PlanNode::distinct(PlanNode::cross(
                join("R.b = S2.b and R.a < S2.c", PlanNode::scan("R"), PlanNode::rename("S2", PlanNode::scan("S"))),
                PlanNode::scan("S"))));
    for (const PlanNode& logical : {aggregates, joins})
    {
        for (std::int64_t frames : {4, 102})
        {
            PlanNode plan = convert(logical, frames).plan;
            CHECK((db::physicalFromJsonText(db::toJsonText(plan)) == plan));
        }
    }
}

TEST_CASE("Fixed algorithm: used when valid, otherwise nested loop")
{
    PlanNode equality = join("R.b = S.b", PlanNode::scan("R"), PlanNode::scan("S"));
    PlanNode lessThan = join("R.a < S.c", PlanNode::scan("R"), PlanNode::scan("S"));
    // At B = 102 nested loop is cheapest, but the fixed choice wins.
    CHECK(convert(equality, 102, JoinAlgorithm::Hash).plan.op == PlanOp::HashJoin);
    CHECK(convert(equality, 102, JoinAlgorithm::SortMerge).plan.op == PlanOp::SortMergeJoin);
    // At B = 4 hash is cheapest, but nested loop is forced.
    CHECK(convert(equality, 4, JoinAlgorithm::NestedLoop).plan.op == PlanOp::NestedLoopJoin);
    CHECK(convert(lessThan, 4, JoinAlgorithm::Hash).plan.op == PlanOp::NestedLoopJoin);
    CHECK(convert(lessThan, 4, JoinAlgorithm::SortMerge).plan.op == PlanOp::NestedLoopJoin);
}

TEST_CASE("Mode of distinct: B changes it, a tie goes to sort")
{
    // No statistics: distinct keeps all 10,000 rows, so the groups need 100
    // pages too.
    PlanNode logical = PlanNode::distinct(PlanNode::scan("R"));
    // B = 4: sort 700; hash 3 x 100 + 2 x 3 = 306 (the groups do not fit
    // in B - 1 = 3 frames).
    CHECK(convert(logical, 4).plan.mode == ExecMode::Hash);
    // B = 102: sort 100 (one pass); hash 100 (the groups fit in 101
    // frames). Equal, so sort.
    CHECK(convert(logical, 102).plan.mode == ExecMode::Sort);
}

TEST_CASE("Mode of group_by: the hash table gets B - 1 frames")
{
    // T is analyzed with ndv(b) = 10,000: 10,000 groups of 8 bytes (key 4 +
    // count 4) need ceil(80,000 / 4096) = 20 pages. The input is 100 pages.
    PlanNode logical =
        PlanNode::groupBy({col("T", "b")}, {Aggregate{AggregateFunction::Count, std::nullopt, "n"}}, PlanNode::scan("T"));
    // B = 21: the table gets 20 frames, the groups fit: hash 100; sort 300
    // (5 runs, one merge pass).
    CHECK(convert(logical, 21).plan.mode == ExecMode::Hash);
    // B = 20: the table gets 19 frames (1 is the scan's page), the groups do
    // not fit: hash 3 x 100 + 2 x 19 = 338; sort 300. With all 20 frames
    // for the table it would wrongly pick hash.
    CHECK(convert(logical, 20).plan.mode == ExecMode::Sort);
}

TEST_CASE("A chain of 998 selects converts (deep trees, kMaxNestingDepth)")
{
    PlanNode logical = PlanNode::scan("R");
    for (int i = 0; i < 998; ++i)
    {
        logical = select("R.a > " + std::to_string(i), std::move(logical));
    }

    PhysicalResult result = convert(logical, 4);

    CHECK(result.estimates.size() == 999);
    const PlanNode* node = &result.plan;
    int filters = 0;
    while (node->op == PlanOp::Filter)
    {
        ++filters;
        node = &node->children.at(0);
    }
    CHECK(filters == 998);
    CHECK(node->op == PlanOp::SeqScan);
}

// ---------------------------------------------------------------------------
// Runs unedited in exec (§M4.6, §M4.9).

namespace
{
    // R(a, b): 200 rows, a = i, b = i mod 50. S(b, c): 50 rows, b = j,
    // c = 10 x j. Every R row matches exactly one S row on b.
    std::unique_ptr<StorageEngine> smallDatabase(const std::string& name)
    {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opt_physical_" + name);
        std::filesystem::remove_all(dir);
        std::unique_ptr<StorageEngine> engine =
            StorageEngine::open(dir, 32, db::storage::ReplacementPolicy::Lru);
        engine->catalog().createTable("R", Schema({{"R", "a", Type::Int, 0}, {"R", "b", Type::Int, 0}}));
        engine->catalog().createTable("S", Schema({{"S", "b", Type::Int, 0}, {"S", "c", Type::Int, 0}}));
        db::storage::HeapFile r = engine->openHeapFile("R");
        for (std::int32_t i = 0; i < 200; ++i)
        {
            r.insert({Value::ofInt(i), Value::ofInt(i % 50)});
        }
        engine->catalog().updateCounts("R", r.numDataPages(), 200);
        db::storage::HeapFile s = engine->openHeapFile("S");
        for (std::int32_t j = 0; j < 50; ++j)
        {
            s.insert({Value::ofInt(j), Value::ofInt(10 * j)});
        }
        engine->catalog().updateCounts("S", s.numDataPages(), 50);
        return engine;
    }

    // Converts `logical` with the catalog's statistics, then runs the
    // physical plan in exec at B = `frames` and returns the number of rows.
    std::size_t runRows(StorageEngine& engine, const PlanNode& logical, std::int64_t frames,
                        std::optional<JoinAlgorithm> fixed = std::nullopt)
    {
        db::opt::StatsProvider stats = [&engine](const std::string& name) { return engine.catalog().getTable(name); };
        PlanNode plan = db::opt::toPhysical(logical, stats, frames, fixed).plan;
        engine.resetPool(static_cast<std::size_t>(frames));
        db::exec::ExecContext ctx(engine);
        db::exec::ExecutablePlan executable = db::exec::buildExecutablePlan(plan, ctx);
        executable.open();
        std::size_t rows = 0;
        db::Tuple tuple;
        while (executable.next(tuple))
        {
            ++rows;
        }
        executable.close();
        return rows;
    }
} // namespace

TEST_CASE("Runs unedited in exec: joins, cross, aggregates, at B = 4 and B = 32")
{
    std::unique_ptr<StorageEngine> engine = smallDatabase("runs");

    // Expected row counts, worked out from the generated rows.
    std::size_t lessThanRows = 0; // pairs with a < c
    for (std::int32_t a = 0; a < 200; ++a)
    {
        for (std::int32_t j = 0; j < 50; ++j)
        {
            if (a < 10 * j)
            {
                ++lessThanRows;
            }
        }
    }

    PlanNode equality = join("e.b = S.b", PlanNode::rename("e", PlanNode::scan("R")), PlanNode::scan("S"));
    PlanNode lessThan = join("R.a < S.c", PlanNode::scan("R"), PlanNode::scan("S"));
    PlanNode mixed = join("R.b = S.b and R.a < S.c", PlanNode::scan("R"), PlanNode::scan("S"));
    PlanNode cross = PlanNode::cross(PlanNode::scan("R"), PlanNode::scan("S"));
    // Each b appears 4 times in R: count(*) = 4 > 3 for all 50 groups.
    PlanNode aggregates = PlanNode::project({col("R", "b"), bare("n"), bare("avg_a")}, select("n > 3", groupByB()));

    for (std::int64_t frames : {4, 32})
    {
        CAPTURE(frames);
        CHECK(runRows(*engine, equality, frames) == 200);
        CHECK(runRows(*engine, equality, frames, JoinAlgorithm::Hash) == 200);
        CHECK(runRows(*engine, equality, frames, JoinAlgorithm::SortMerge) == 200);
        CHECK(runRows(*engine, lessThan, frames) == lessThanRows);
        CHECK(runRows(*engine, lessThan, frames, JoinAlgorithm::Hash) == lessThanRows);
        // b = j matches R rows j, j + 50, j + 100, j + 150; a < 10 x j.
        std::size_t mixedRows = 0;
        for (std::int32_t a = 0; a < 200; ++a)
        {
            mixedRows += (a < 10 * (a % 50)) ? 1 : 0;
        }
        CHECK(runRows(*engine, mixed, frames, JoinAlgorithm::Hash) == mixedRows);
        CHECK(runRows(*engine, cross, frames) == 200 * 50);
        CHECK(runRows(*engine, aggregates, frames) == 50);
        CHECK(runRows(*engine, PlanNode::distinct(PlanNode::project({col("R", "b")}, PlanNode::scan("R"))),
                      frames) == 50);
    }
}

