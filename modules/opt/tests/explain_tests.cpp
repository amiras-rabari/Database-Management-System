// Chunk 13: the explain text and the DP table text (§M4.6, §M4.3, §M4.5).
//
// Made-up tables (never on disk), as in the planner tests:
// - Emp(name, did, age) 10,000 rows on 200 pages and Dept(did, budget) 50
//   rows on 1 page, never analyzed: the §M4.3 example.
// - A(x), Bp(x, y), C(y): the §M4.5 example (B' is written Bp; a quote
//   cannot be part of a name).
// - R(a, b): 10,000 rows on 100 pages, for the deep-tree test.

#include <doctest/doctest.h>

#include <db/common/condition.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/common/value.h>
#include <db/opt/enumerate.h>
#include <db/opt/estimate.h>
#include <db/opt/explain.h>
#include <db/opt/optimizer.h>
#include <db/opt/physical.h>
#include <db/storage/table_info.h>
#include <db/storage/table_stats.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using db::Aggregate;
    using db::AggregateFunction;
    using db::ColumnRef;
    using db::PlanNode;
    using db::PlanOp;
    using db::Schema;
    using db::Type;
    using db::Value;
    using db::opt::DpTable;
    using db::opt::JoinGraph;
    using db::opt::JoinPredicate;
    using db::opt::JoinRelation;
    using db::opt::NodeEstimate;
    using db::opt::OptimizedPlan;
    using db::opt::OptimizerOptions;
    using db::storage::ColumnStats;
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

    // A physical node with a condition (filter or a join) and its inputs.
    PlanNode physical(PlanOp op, const std::string& condition, std::vector<PlanNode> children)
    {
        PlanNode node;
        node.op = op;
        node.condition = db::parseCondition(condition);
        node.children = std::move(children);
        return node;
    }

    PlanNode seqscan(const std::string& table)
    {
        PlanNode node;
        node.op = PlanOp::SeqScan;
        node.table = table;
        return node;
    }

    // A table of INT columns (the first one VARCHAR(20) when `varcharFirst`).
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
            if (name == "R")
            {
                return fakeTable("R", {"a", "b"}, 100, 10000);
            }
            return std::nullopt;
        };
    }

    OptimizedPlan optimizeAt(const PlanNode& logical, std::int64_t frames)
    {
        OptimizerOptions options;
        options.frames = frames;
        return db::opt::optimize(logical, fakeStats(), options);
    }

    std::vector<std::string> linesOf(const std::string& text)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            lines.push_back(line);
        }
        return lines;
    }

    // The indented label of every line: the text before `est_rows=`, with
    // the padding spaces taken off.
    std::vector<std::string> labelsOf(const std::string& text)
    {
        std::vector<std::string> labels;
        for (const std::string& line : linesOf(text))
        {
            std::string label = line.substr(0, line.find("est_rows="));
            while (!label.empty() && label.back() == ' ')
            {
                label.pop_back();
            }
            labels.push_back(label);
        }
        return labels;
    }

    std::size_t nodeCount(const PlanNode& plan)
    {
        std::size_t count = 1;
        for (const PlanNode& child : plan.children)
        {
            count += nodeCount(child);
        }
        return count;
    }

    // The label of the root of `plan` alone.
    std::string rootLabel(const PlanNode& plan)
    {
        std::vector<NodeEstimate> estimates(nodeCount(plan), NodeEstimate{1, 1, 4.0, 0});
        return labelsOf(db::opt::explainText(plan, estimates, false)).front();
    }

    // The §M4.5 example as a DP input (the same as the DP tests).
    JoinGraph specGraph()
    {
        JoinGraph graph;
        graph.relations.push_back(JoinRelation{"A", 100, 100, 5000});
        graph.relations.push_back(JoinRelation{"B'", 10, 10, 500});
        graph.relations.push_back(JoinRelation{"C", 400, 400, 20000});
        graph.predicates.push_back(JoinPredicate{db::parseCondition("A.x = Bp.x"), 0b011, true});
        graph.predicates.push_back(JoinPredicate{db::parseCondition("Bp.y = C.y"), 0b110, true});
        return graph;
    }

    std::vector<NodeEstimate> specSizes()
    {
        std::vector<NodeEstimate> sizes(8);
        sizes[0b011] = NodeEstimate{1000, 20, 0.0, 0};
        sizes[0b110] = NodeEstimate{2000, 40, 0.0, 0};
        sizes[0b111] = NodeEstimate{1000, 20, 0.0, 0};
        return sizes;
    }
} // namespace

TEST_CASE("The §M4.6 explain block, character for character")
{
    // The spec's tree with the spec's numbers put in by hand: this tests the
    // layout, not the estimates (330 cannot come from the inputs shown).
    PlanNode plan = physical(PlanOp::HashJoin, "R.b = S.b",
                             {physical(PlanOp::Filter, "R.a > 500", {seqscan("R")}), seqscan("S")});
    std::vector<NodeEstimate> estimates = {
        NodeEstimate{2000, 0, 0.0, 330},
        NodeEstimate{1000, 0, 0.0, 0},
        NodeEstimate{10000, 0, 0.0, 200},
        NodeEstimate{500, 0, 0.0, 10},
    };

    std::string expected = "hash_join (R.b = S.b)        est_rows=2000   est_cost=330\n"
                           "  filter (R.a > 500)         est_rows=1000   est_cost=0\n"
                           "    seqscan R                est_rows=10000  est_cost=200\n"
                           "  seqscan S                  est_rows=500    est_cost=10\n";
    CHECK(db::opt::explainText(plan, estimates, true) == expected);

    // Without the cost column the rows column stays where it was.
    std::string rowsOnly = "hash_join (R.b = S.b)        est_rows=2000\n"
                           "  filter (R.a > 500)         est_rows=1000\n"
                           "    seqscan R                est_rows=10000\n"
                           "  seqscan S                  est_rows=500\n";
    CHECK(db::opt::explainText(plan, estimates, false) == rowsOnly);
}

TEST_CASE("Every physical line carries est_rows and est_cost, one line per estimate (§M4.6)")
{
    PlanNode logical = select("A.x = Bp.x and Bp.y = C.y",
                              PlanNode::cross(PlanNode::cross(PlanNode::scan("A"), PlanNode::scan("C")),
                                              PlanNode::scan("Bp")));
    OptimizedPlan result = optimizeAt(logical, 32);

    std::string text = db::opt::explainText(result.physical, result.estimates, true);
    std::vector<std::string> lines = linesOf(text);
    CHECK(lines.size() == result.estimates.size());
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        CAPTURE(lines[i]);
        CHECK(lines[i].find("est_rows=" + std::to_string(result.estimates[i].rows)) != std::string::npos);
        CHECK(lines[i].find("est_cost=" + std::to_string(result.estimates[i].cost)) != std::string::npos);
    }

    // Formatting twice gives the same text.
    CHECK(db::opt::explainText(result.physical, result.estimates, true) == text);

    // A logical tree: est_rows on every line, no est_cost.
    std::string before =
        db::opt::explainText(result.logicalBefore, db::opt::estimatePlan(result.logicalBefore, fakeStats()), false);
    for (const std::string& line : linesOf(before))
    {
        CAPTURE(line);
        CHECK(line.find("est_rows=") != std::string::npos);
        CHECK(line.find("est_cost=") == std::string::npos);
    }
}

TEST_CASE("The §M4.3 example: the trees before and after the rewrite rules")
{
    // The spec's "before" tree.
    PlanNode logical = PlanNode::project(
        {col("e", "name")},
        select("e.did = d.did",
               select("e.age > 60",
                      select("d.budget > 1000", PlanNode::cross(PlanNode::rename("e", PlanNode::scan("Emp")),
                                                                PlanNode::rename("d", PlanNode::scan("Dept")))))));
    OptimizedPlan result = optimizeAt(logical, 32);

    std::string before =
        db::opt::explainText(result.logicalBefore, db::opt::estimatePlan(result.logicalBefore, fakeStats()), false);
    CHECK(labelsOf(before) == std::vector<std::string>{
                                  "project [e.name]",
                                  "  select (e.did = d.did)",
                                  "    select (e.age > 60)",
                                  "      select (d.budget > 1000)",
                                  "        cross",
                                  "          rename e",
                                  "            scan Emp",
                                  "          rename d",
                                  "            scan Dept",
                              });

    std::string after =
        db::opt::explainText(result.logicalAfter, db::opt::estimatePlan(result.logicalAfter, fakeStats()), false);
    CHECK(labelsOf(after) == std::vector<std::string>{
                                 "project [e.name]",
                                 "  join (e.did = d.did)",
                                 "    project [e.name, e.did]",
                                 "      select (e.age > 60)",
                                 "        rename e",
                                 "          scan Emp",
                                 "    project [d.did]",
                                 "      select (d.budget > 1000)",
                                 "        rename d",
                                 "          scan Dept",
                             });
    // The rewrite's point, in numbers: 500,000 rows from the cross before,
    // far fewer at the join after (no statistics: documented defaults).
    std::vector<std::string> beforeLines = linesOf(before);
    CHECK(beforeLines[4].find("est_rows=500000") != std::string::npos);
    std::vector<std::string> afterLines = linesOf(after);
    CAPTURE(after);
    CHECK(afterLines[1].find("est_rows=5668") != std::string::npos);
}

TEST_CASE("The §M4.5 DP table prints in full (B = 32)")
{
    DpTable table = db::opt::enumerateJoins(specGraph(), specSizes(), 32);
    std::string expected = "Join block: A, B', C\n"
                           "\n"
                           "| Subset | Best plan considered | Cost |\n"
                           "|---|---|---|\n"
                           "| {A} | scan A | 100 |\n"
                           "| {B'} | scan B' | 10 |\n"
                           "| {C} | scan C | 400 |\n"
                           "| {A,B'} | B' outer, BNL with A inner | 110 |\n"
                           "| {A,C} | no predicate, skipped | — |\n"
                           "| {B',C} | B' outer, BNL with C inner | 410 |\n"
                           "| {A,B',C} | {A,B'} outer (20 pages), BNL with C inner | 530 |\n"
                           "| | alternative: {B',C} outer (40 pages), BNL with A inner | 650 |\n";
    CHECK(db::opt::dpTableText(table) == expected);
}

TEST_CASE("DP table: B = 4 shows hash joins; more than six relations says so")
{
    // At B = 4 the DP picks hash joins (chunk 11's test): the text names them.
    std::string small = db::opt::dpTableText(db::opt::enumerateJoins(specGraph(), specSizes(), 4));
    CHECK(small.find("| {A,B'} | B' outer, hash with A inner | 330 |") != std::string::npos);
    CHECK(small.find("| {A,B',C} | {A,B'} outer (20 pages), hash with C inner | 1590 |") != std::string::npos);

    // A table made by hand where sort-merge won.
    DpTable merged;
    merged.names = {"R", "S"};
    merged.entries.push_back(db::opt::DpEntry{0b01, false, 100, 1000, 100, {}, 0});
    merged.entries.push_back(db::opt::DpEntry{0b10, false, 100, 1000, 100, {}, 0});
    merged.entries.push_back(db::opt::DpEntry{
        0b11, false, 200, 1000, 50, {db::opt::DpCandidate{0b01, 1, db::opt::JoinAlgorithm::SortMerge, 200}}, 0});
    CHECK(db::opt::dpTableText(merged).find("| {R,S} | R outer, sort-merge with S inner | 200 |\n") !=
          std::string::npos);

    DpTable seven;
    seven.names = {"T1", "T2", "T3", "T4", "T5", "T6", "T7"};
    CHECK(db::opt::dpTableText(seven) == "Join block: T1, T2, T3, T4, T5, T6, T7\n\n"
                                         "More than six relations: no DP; the joins keep their written order.\n");
}

TEST_CASE("A cross product prints as nested_loop_join (1 = 1)")
{
    OptimizedPlan result = optimizeAt(PlanNode::cross(PlanNode::scan("A"), PlanNode::scan("C")), 32);
    std::string text = db::opt::explainText(result.physical, result.estimates, true);
    CAPTURE(text);
    CHECK(text.find("nested_loop_join (1 = 1)") != std::string::npos);
}

TEST_CASE("Labels of the other operators")
{
    PlanNode scanR = seqscan("R");

    PlanNode sort;
    sort.op = PlanOp::Sort;
    sort.sortKeys = {db::SortKey{col("R", "a"), true}, db::SortKey{col("R", "b"), false}};
    sort.children = {scanR};
    CHECK(rootLabel(sort) == "sort [R.a asc, R.b desc]");

    // Physical group_by: keys, aggregates without aliases, the mode.
    PlanNode group;
    group.op = PlanOp::GroupBy;
    group.columns = {col("R", "a")};
    group.aggregates = {Aggregate{AggregateFunction::Count, std::nullopt, ""},
                        Aggregate{AggregateFunction::Avg, col("R", "b"), ""}};
    group.mode = db::ExecMode::Hash;
    group.children = {scanR};
    CHECK(rootLabel(group) == "group_by [R.a; count(*), avg(R.b)] mode=hash");

    // Logical group_by with no keys: the alias, no mode.
    PlanNode logicalGroup =
        PlanNode::groupBy({}, {Aggregate{AggregateFunction::Count, std::nullopt, "n"}}, PlanNode::scan("R"));
    CHECK(rootLabel(logicalGroup) == "group_by [count(*) as n]");

    PlanNode distinct;
    distinct.op = PlanOp::Distinct;
    distinct.mode = db::ExecMode::Sort;
    distinct.children = {scanR};
    CHECK(rootLabel(distinct) == "distinct mode=sort");
    CHECK(rootLabel(PlanNode::distinct(PlanNode::scan("R"))) == "distinct");

    CHECK(rootLabel(PlanNode::limit(10, PlanNode::scan("R"))) == "limit 10");
    CHECK(rootLabel(PlanNode::unionAll(PlanNode::scan("R"), PlanNode::scan("R"))) == "union_all");
    CHECK(rootLabel(PlanNode::rename("t", PlanNode::scan("R"))) == "rename t");
    CHECK(rootLabel(PlanNode::project({col("R", "a"), col("R", "b")}, PlanNode::scan("R"))) ==
          "project [R.a, R.b]");
}

TEST_CASE("Deep trees: 998 selects print (kMaxNestingDepth)")
{
    PlanNode plan = PlanNode::scan("R");
    for (int i = 0; i < 998; ++i)
    {
        plan = select("R.a > 5", std::move(plan));
    }
    std::string text = db::opt::explainText(plan, db::opt::estimatePlan(plan, fakeStats()), false);
    std::vector<std::string> lines = linesOf(text);
    REQUIRE(lines.size() == 999);
    CHECK(lines.back().rfind(std::string(2 * 998, ' ') + "scan R", 0) == 0);
}
