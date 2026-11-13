// Chunk 11: the Selinger join-order DP over left-deep trees (§M4.5).
//
// The golden test is the spec's own example at B = 32: A (100 pages), B'
// (10 pages), C (400 pages), with A.x = B'.x and B'.y = C.y and nothing
// between A and C. Relations are numbered in written order: A = 0, B' = 1,
// C = 2, so a group is a 3-bit number: {A} = 1, {B'} = 2, {A,B'} = 3,
// {C} = 4, {A,C} = 5, {B',C} = 6, {A,B',C} = 7. In conditions B' is
// written `Bp` (a quote cannot be part of a name).

#include <doctest/doctest.h>

#include <db/common/condition.h>
#include <db/opt/enumerate.h>
#include <db/opt/estimate.h>
#include <db/opt/physical.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    using db::opt::DpCandidate;
    using db::opt::DpEntry;
    using db::opt::DpTable;
    using db::opt::JoinAlgorithm;
    using db::opt::JoinGraph;
    using db::opt::JoinPredicate;
    using db::opt::JoinRelation;
    using db::opt::NodeEstimate;

    const JoinAlgorithm kNestedLoop = JoinAlgorithm::NestedLoop;
    const JoinAlgorithm kSortMerge = JoinAlgorithm::SortMerge;
    const JoinAlgorithm kHash = JoinAlgorithm::Hash;

    JoinPredicate equality(const std::string& condition, std::uint32_t relations)
    {
        return JoinPredicate{db::parseCondition(condition), relations, true};
    }

    // The §M4.5 example.
    JoinGraph specGraph()
    {
        JoinGraph graph;
        graph.relations.push_back(JoinRelation{"A", 100, 100, 5000});
        graph.relations.push_back(JoinRelation{"B'", 10, 10, 500});
        graph.relations.push_back(JoinRelation{"C", 400, 400, 20000});
        graph.predicates.push_back(equality("A.x = Bp.x", 0b011));
        graph.predicates.push_back(equality("Bp.y = C.y", 0b110));
        return graph;
    }

    // The spec's sizes of the joined groups: {A,B'} 1,000 rows on 20 pages,
    // {B',C} 2,000 rows on 40 pages. The others are never read for a cost
    // ({A,C} has no plan; the full group is never an outer).
    std::vector<NodeEstimate> specSizes()
    {
        std::vector<NodeEstimate> sizes(8);
        sizes[0b011] = NodeEstimate{1000, 20, 0.0, 0};
        sizes[0b110] = NodeEstimate{2000, 40, 0.0, 0};
        sizes[0b111] = NodeEstimate{1000, 20, 0.0, 0};
        return sizes;
    }

    // The entry of group `subset`, wherever it is in the table.
    const DpEntry& entryOf(const DpTable& table, std::uint32_t subset)
    {
        for (const DpEntry& entry : table.entries)
        {
            if (entry.subset == subset)
            {
                return entry;
            }
        }
        FAIL("no entry for subset " << subset);
        return table.entries.front();
    }

    const DpCandidate& winnerOf(const DpEntry& entry)
    {
        REQUIRE(!entry.candidates.empty());
        return entry.candidates[entry.chosen];
    }
} // namespace

TEST_CASE("The §M4.5 DP table reproduces at B = 32")
{
    DpTable table = db::opt::enumerateJoins(specGraph(), specSizes(), 32);

    // One entry per group: 3 single relations, 3 pairs, 1 triple, visited
    // by size, then in number order.
    REQUIRE(table.entries.size() == 7);
    std::vector<std::uint32_t> order;
    for (const DpEntry& entry : table.entries)
    {
        order.push_back(entry.subset);
    }
    CHECK(order == std::vector<std::uint32_t>{0b001, 0b010, 0b100, 0b011, 0b101, 0b110, 0b111});
    CHECK(table.names == std::vector<std::string>{"A", "B'", "C"});

    // {A} 100, {B'} 10, {C} 400: reading each table once.
    CHECK(entryOf(table, 0b001).cost == 100);
    CHECK(entryOf(table, 0b010).cost == 10);
    CHECK(entryOf(table, 0b100).cost == 400);
    CHECK(entryOf(table, 0b001).candidates.empty());

    // {A,B'}: B' outer, nested loop with A inner, 10 + 1 x 100 = 110.
    // Every candidate, in the order tried (inner A first, then inner B'):
    const DpEntry& ab = entryOf(table, 0b011);
    CHECK(ab.cost == 110);
    CHECK(ab.rows == 1000);
    CHECK(ab.pages == 20);
    CHECK(ab.candidates == std::vector<DpCandidate>{
                               {0b010, 0, kNestedLoop, 110}, // B' outer: 10 + ceil(10/30) x 100
                               {0b010, 0, kSortMerge, 310},  // sort(10) + sort(100) = 10 + 300
                               {0b010, 0, kHash, 330},       // 3 x (10 + 100)
                               {0b001, 1, kNestedLoop, 140}, // A outer: 100 + ceil(100/30) x 10
                               {0b001, 1, kSortMerge, 310},
                               {0b001, 1, kHash, 330},
                           });
    CHECK(winnerOf(ab) == DpCandidate{0b010, 0, kNestedLoop, 110});

    // {A,C}: no predicate between them, skipped.
    const DpEntry& ac = entryOf(table, 0b101);
    CHECK(ac.skipped);
    CHECK(ac.candidates.empty());

    // {B',C}: B' outer, nested loop with C inner, 10 + 1 x 400 = 410.
    const DpEntry& bc = entryOf(table, 0b110);
    CHECK(bc.cost == 410);
    CHECK(winnerOf(bc) == DpCandidate{0b010, 2, kNestedLoop, 410});

    // {A,B',C}: {A,B'} outer (20 pages), nested loop with C inner:
    // 110 + 20 + 1 x 400 = 530. The alternative, {B',C} outer (40 pages)
    // with A inner, is 410 + 40 + ceil(40/30) x 100 = 650 (the spec prints
    // 550; ceil(40/30) is 2, not 1). {A,C} has no plan, so it is never an
    // outer.
    const DpEntry& all = entryOf(table, 0b111);
    CHECK(all.cost == 530);
    CHECK(winnerOf(all) == DpCandidate{0b011, 2, kNestedLoop, 530});
    CHECK(all.candidates == std::vector<DpCandidate>{
                                {0b110, 0, kNestedLoop, 650},
                                {0b110, 0, kSortMerge, 830},  // 410 + sort(40) 120 + sort(100) 300
                                {0b110, 0, kHash, 830},       // 410 + 3 x (40 + 100)
                                {0b011, 2, kNestedLoop, 530},
                                {0b011, 2, kSortMerge, 1330}, // 110 + sort(20) 20 + sort(400) 1,200
                                {0b011, 2, kHash, 1370},      // 110 + 3 x (20 + 400)
                            });
}

TEST_CASE("The answer as join steps: (B' join A) join C, each with its own condition")
{
    DpTable table = db::opt::enumerateJoins(specGraph(), specSizes(), 32);

    CHECK(table.first == 1); // B'
    REQUIRE(table.steps.size() == 2);
    CHECK(table.steps[0].inner == 0); // then A
    CHECK(table.steps[0].algorithm == kNestedLoop);
    CHECK((table.steps[0].conditions == std::vector<db::Condition>{db::parseCondition("A.x = Bp.x")}));
    CHECK(table.steps[1].inner == 2); // then C
    CHECK(table.steps[1].algorithm == kNestedLoop);
    CHECK((table.steps[1].conditions == std::vector<db::Condition>{db::parseCondition("Bp.y = C.y")}));
}

TEST_CASE("B = 4 changes the choice: hash wins, and a tie goes to the lower inner index")
{
    DpTable table = db::opt::enumerateJoins(specGraph(), specSizes(), 4);

    // {A,B'}: nested loop 10 + 5 x 100 = 510 or 100 + 50 x 10 = 600;
    // sort-merge sort(10) 30 + sort(100) 700 = 730; hash 330 both ways.
    // The first hash tried (inner A) wins the tie.
    const DpEntry& ab = entryOf(table, 0b011);
    CHECK(winnerOf(ab) == DpCandidate{0b010, 0, kHash, 330});

    // {A,B',C}: {A,B'} outer with hash 330 + 3 x (20 + 400) = 1,590,
    // against {B',C} outer (best 1,230) with hash 1,230 + 3 x 140 = 1,650.
    const DpEntry& all = entryOf(table, 0b111);
    CHECK(all.cost == 1590);
    CHECK(winnerOf(all) == DpCandidate{0b011, 2, kHash, 1590});
    CHECK(table.steps[0].algorithm == kHash);
    CHECK(table.steps[1].algorithm == kHash);
}

TEST_CASE("Sort-merge and hash need an equality between the two sides")
{
    // The same graph, but A.x < B'.x is not an equality: only nested loop
    // for {A,B'} (and for the full group when A is the one added).
    JoinGraph graph = specGraph();
    graph.predicates[0] = JoinPredicate{db::parseCondition("A.x < Bp.x"), 0b011, false};

    DpTable table = db::opt::enumerateJoins(graph, specSizes(), 4);

    for (const DpCandidate& candidate : entryOf(table, 0b011).candidates)
    {
        CHECK(candidate.algorithm == kNestedLoop);
    }
    // {A,B',C} with A added last links only through A.x < B'.x.
    for (const DpCandidate& candidate : entryOf(table, 0b111).candidates)
    {
        if (candidate.inner == 0)
        {
            CHECK(candidate.algorithm == kNestedLoop);
        }
    }
}

TEST_CASE("Unconnected relations still get a plan (a cross product, nested loop only)")
{
    SUBCASE("two relations with no predicate")
    {
        JoinGraph graph;
        graph.relations.push_back(JoinRelation{"R", 100, 100, 10000});
        graph.relations.push_back(JoinRelation{"S", 10, 10, 1000});
        std::vector<NodeEstimate> sizes(4);
        sizes[0b11] = NodeEstimate{10000000, 1000, 0.0, 0};

        DpTable table = db::opt::enumerateJoins(graph, sizes, 32);

        // S outer: 10 + 1 x 100 = 110; R outer: 100 + 4 x 10 = 140.
        const DpEntry& rs = entryOf(table, 0b11);
        CHECK(rs.candidates == std::vector<DpCandidate>{{0b10, 0, kNestedLoop, 110}, {0b01, 1, kNestedLoop, 140}});
        CHECK(table.first == 1);
        REQUIRE(table.steps.size() == 1);
        CHECK(table.steps[0].inner == 0);
        CHECK(table.steps[0].conditions.empty()); // a cross: no condition
    }

    SUBCASE("A and B joined, C unconnected")
    {
        JoinGraph graph;
        graph.relations.push_back(JoinRelation{"A", 10, 10, 1000});
        graph.relations.push_back(JoinRelation{"B", 10, 10, 1000});
        graph.relations.push_back(JoinRelation{"C", 10, 10, 1000});
        graph.predicates.push_back(equality("A.x = B.x", 0b011));
        std::vector<NodeEstimate> sizes(8, NodeEstimate{1000, 10, 0.0, 0});

        DpTable table = db::opt::enumerateJoins(graph, sizes, 32);

        // {A,C} and {B,C} have no predicate, but nothing else could ever
        // bring C in, so they are built as crosses instead of skipped.
        CHECK(!entryOf(table, 0b101).skipped);
        CHECK(!entryOf(table, 0b110).skipped);
        // The full group still prefers pairings that have a predicate:
        // {A,B} outer with C inner has none, so it is not tried.
        const DpEntry& all = entryOf(table, 0b111);
        CHECK(!all.skipped);
        for (const DpCandidate& candidate : all.candidates)
        {
            CHECK(candidate.outer != 0b011);
        }
        CHECK(table.steps.size() == 2);
    }
}

TEST_CASE("Six relations in a chain: 63 groups, only the connected ones are built")
{
    // R0 - R1 - R2 - R3 - R4 - R5, each joined to the next on k.
    JoinGraph graph;
    for (int i = 0; i < 6; ++i)
    {
        graph.relations.push_back(JoinRelation{"R" + std::to_string(i), 10, 10, 100});
    }
    for (int i = 0; i < 5; ++i)
    {
        std::string condition = "R" + std::to_string(i) + ".k = R" + std::to_string(i + 1) + ".k";
        graph.predicates.push_back(equality(condition, (1u << i) | (1u << (i + 1))));
    }
    std::vector<NodeEstimate> sizes(64, NodeEstimate{100, 10, 0.0, 0});

    DpTable table = db::opt::enumerateJoins(graph, sizes, 32);

    CHECK(table.entries.size() == 63);
    // In a chain, a group can be built only when its members sit next to
    // each other (R1 R2 R3, not R1 R3): 6 + 5 + 4 + 3 + 2 + 1 = 21 groups.
    int built = 0;
    for (const DpEntry& entry : table.entries)
    {
        if (!entry.skipped)
        {
            ++built;
        }
    }
    CHECK(built == 21);
    CHECK(!entryOf(table, 0b111111).skipped);
    CHECK(table.steps.size() == 5);
}

TEST_CASE("One relation: no joins")
{
    JoinGraph graph;
    graph.relations.push_back(JoinRelation{"R", 100, 100, 10000});

    DpTable table = db::opt::enumerateJoins(graph, std::vector<NodeEstimate>(2), 32);

    CHECK(table.entries.size() == 1);
    CHECK(table.first == 0);
    CHECK(table.steps.empty());
}

TEST_CASE("Same input, same table")
{
    CHECK(db::opt::enumerateJoins(specGraph(), specSizes(), 32) ==
          db::opt::enumerateJoins(specGraph(), specSizes(), 32));
}
