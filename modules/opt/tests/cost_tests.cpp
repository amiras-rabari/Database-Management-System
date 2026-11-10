// Chunk 09: the §M4.4 cost formulas, checked against the spec's numbers
// (the §M4.5 DP table) and exec's measured sort.

#include <doctest/doctest.h>

#include <db/opt/cost.h>

using db::opt::costHashGroup;
using db::opt::costHashJoin;
using db::opt::costNestedLoopJoin;
using db::opt::costSort;
using db::opt::costSortMergeJoin;

TEST_CASE("sort: exec's measured example")
{
    // N = 8, B = 3: 3 runs, then 2 merge passes; exec measures 24 reads +
    // 16 writes (rewrite variant, DECISIONS 2026-11-06).
    CHECK(costSort(8, 3) == 40);
}

TEST_CASE("sort at B = 32")
{
    CHECK(costSort(100, 32) == 300); // 4 runs, one merge pass: 2 passes
    CHECK(costSort(10, 32) == 10); // fits: one pass reads it once
    CHECK(costSort(32, 32) == 32);
    CHECK(costSort(33, 32) == 99); // 2 runs: 2 passes
}

TEST_CASE("sort: whole-number passes at exact powers")
{
    // 9 runs at B = 4 merge 3 at a time: 9 -> 3 -> 1, so 3 passes. A
    // floating log(9)/log(3) can come out a hair above 2 and add a pass.
    CHECK(costSort(36, 4) == 180);
    // 250 runs: 3^5 = 243 < 250 <= 3^6, so 6 merge passes, 7 in all.
    CHECK(costSort(1000, 4) == 13000);
    // B = 6: 750 pages make 125 = 5^3 runs, so 125 -> 25 -> 5 -> 1 is 3
    // merge passes, 4 in all: 2 x 750 x 3 + 750. In doubles log(125)/log(5)
    // is 3.0000000000000004, and a ceil of it would add a fifth pass (6750).
    CHECK(costSort(750, 6) == 5250);
}

TEST_CASE("joins: the cells of the §M4.5 DP table at B = 32")
{
    // {A,B'}: B' (10 pages) outer, A (100) inner: 10 + 1 x 100.
    CHECK(costNestedLoopJoin(10, 100, 32) == 110);
    // The other way round: 100 + ceil(100/30) x 10.
    CHECK(costNestedLoopJoin(100, 10, 32) == 140);
    // {B',C}: 10 + 1 x 400.
    CHECK(costNestedLoopJoin(10, 400, 32) == 410);
    // {A,B',C}: {A,B'} costs 110 and outputs 20 pages; C inner:
    // 110 + 20 + 1 x 400.
    CHECK(110 + costNestedLoopJoin(20, 400, 32) == 530);
    // The alternative, {B',C} (410, 40 pages) outer, A inner. The spec
    // prints 550, but ceil(40/30) = 2: 410 + 40 + 2 x 100 = 650.
    CHECK(410 + costNestedLoopJoin(40, 100, 32) == 650);

    CHECK(costHashJoin(10, 100) == 330);
    // sort(100) + sort(10) = 300 + 10; §M4.4's "+ M + N" would give 420.
    CHECK(costSortMergeJoin(100, 10, 32) == 310);
}

TEST_CASE("B changes the cheapest join")
{
    // M = N = 100. Large B: the outer fits in one block.
    CHECK(costNestedLoopJoin(100, 100, 102) == 200);
    CHECK(costHashJoin(100, 100) == 600);
    // B = 4: blocks of 2 pages, 50 passes over the inner.
    CHECK(costNestedLoopJoin(100, 100, 4) == 5100);
}

TEST_CASE("hash-mode distinct and group_by")
{
    // 50 groups on 1 page fit a 31-page table: read the input once.
    CHECK(costHashGroup(100, 1, 31) == 100);
    CHECK(costHashGroup(100, 31, 31) == 100);
    // 50 pages of groups do not fit: 3 x 100 + 2 x 31 (spill written and read).
    CHECK(costHashGroup(100, 50, 31) == 362);
}
