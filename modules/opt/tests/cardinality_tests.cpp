// Chunk 05: estimated rows, width and pages of every plan node (§M4.2).
//
// Emp and Dept are the spec's made-up tables, built in memory: Emp has
// 10,000 tuples on 200 pages, ndv(did) = 50, age 20..70; Dept has 50 tuples
// on 1 page, ndv(did) = 50. No disk file is involved.

#include <doctest/doctest.h>

#include <db/common/condition.h>
#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/schema.h>
#include <db/common/value.h>
#include <db/opt/estimate.h>
#include <db/storage/table_info.h>
#include <db/storage/table_stats.h>

#include <cmath>
#include <cstdint>
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
    using db::Value;
    using db::opt::NodeEstimate;
    using db::storage::ColumnStats;
    using db::storage::HistogramBucket;
    using db::storage::TableInfo;
    using db::storage::TableStats;

    ColumnStats intColumn(std::uint64_t ndv, std::int32_t min, std::int32_t max)
    {
        return ColumnStats{ndv, Value::ofInt(min), Value::ofInt(max), {}};
    }

    // Emp(eid, name VARCHAR(20), did, age, salary): 10,000 rows on 200 pages,
    // so width = 200 x 4096 / 10,000 = 81.92 bytes. did has one histogram
    // bucket [1, 50] (10,000 rows, 50 values); a value outside it matches
    // nothing.
    TableInfo emp(bool analyzed)
    {
        Schema schema({{"Emp", "eid", Type::Int, 0},
                       {"Emp", "name", Type::Varchar, 20},
                       {"Emp", "did", Type::Int, 0},
                       {"Emp", "age", Type::Int, 0},
                       {"Emp", "salary", Type::Int, 0}});
        TableInfo info{"Emp", schema, "Emp.db", 0, 200, 10000, std::nullopt};
        if (analyzed)
        {
            ColumnStats did = intColumn(50, 1, 50);
            did.histogram.push_back(HistogramBucket{Value::ofInt(1), Value::ofInt(50), 10000, 50});
            ColumnStats name{10000, Value::ofVarchar("Aaron"), Value::ofVarchar("Zoe"), {}};
            info.stats = TableStats{10000, 200, 81.92,
                                    {intColumn(10000, 1, 10000), name, did, intColumn(51, 20, 70),
                                     intColumn(1000, 1000, 9000)}};
        }
        return info;
    }

    // Dept(did, dname VARCHAR(20), budget): 50 rows on 1 page.
    TableInfo dept(bool analyzed)
    {
        Schema schema({{"Dept", "did", Type::Int, 0},
                       {"Dept", "dname", Type::Varchar, 20},
                       {"Dept", "budget", Type::Int, 0}});
        TableInfo info{"Dept", schema, "Dept.db", 0, 1, 50, std::nullopt};
        if (analyzed)
        {
            ColumnStats dname{50, Value::ofVarchar("Art"), Value::ofVarchar("Zoo"), {}};
            info.stats = TableStats{50, 1, 81.92, {intColumn(50, 1, 50), dname, intColumn(50, 100, 5000)}};
        }
        return info;
    }

    // A provider that knows Emp and Dept, analyzed or not.
    db::opt::StatsProvider provider(bool empAnalyzed, bool deptAnalyzed)
    {
        return [empAnalyzed, deptAnalyzed](const std::string& name) -> std::optional<TableInfo> {
            if (name == "Emp")
            {
                return emp(empAnalyzed);
            }
            if (name == "Dept")
            {
                return dept(deptAnalyzed);
            }
            return std::nullopt;
        };
    }

    // The root's estimate, with both tables analyzed.
    NodeEstimate root(const PlanNode& plan)
    {
        return db::opt::estimatePlan(plan, provider(true, true)).at(0);
    }

    PlanNode selectOn(const std::string& condition, PlanNode child)
    {
        return PlanNode::select(db::parseCondition(condition), std::move(child));
    }

    ColumnRef col(const std::string& qualifier, const std::string& name)
    {
        return ColumnRef{qualifier, name};
    }
} // namespace

TEST_CASE("§M4.2 worked example: 200, 2000, 40 and 10,000 rows")
{
    PlanNode scanEmp = PlanNode::scan("Emp");

    CHECK(root(selectOn("Emp.did = 7", scanEmp)).rows == 200);
    CHECK(root(selectOn("Emp.age > 60", scanEmp)).rows == 2000);
    CHECK(root(selectOn("Emp.did = 7 and Emp.age > 60", scanEmp)).rows == 40);
    // Two stacked selects multiply the same way: 200 x 0.2 = 40.
    CHECK(root(selectOn("Emp.age > 60", selectOn("Emp.did = 7", scanEmp))).rows == 40);

    // 10,000 x 50 / max(50, 50).
    PlanNode join = PlanNode::join(db::parseCondition("Emp.did = Dept.did"), scanEmp, PlanNode::scan("Dept"));
    CHECK(root(join).rows == 10000);
}

TEST_CASE("pages = ceil(rows x width / 4096); a scan gives back the table's pages")
{
    PlanNode scanEmp = PlanNode::scan("Emp");

    NodeEstimate scan = root(scanEmp);
    CHECK(scan.rows == 10000);
    CHECK(scan.pages == 200);
    CHECK(scan.width == doctest::Approx(81.92));
    CHECK(scan.cost == 0);

    // 200 x 81.92 / 4096 = 4 exactly on paper; in doubles a hair above 4.
    CHECK(root(selectOn("Emp.did = 7", scanEmp)).pages == 4);
    CHECK(root(selectOn("Emp.age > 60", scanEmp)).pages == 40);
}

TEST_CASE("a scan reads the catalog's real pages, not rows x width / 4096")
{
    // ANALYZE's width counts record bytes only (30 here), not page headers
    // or free space; 10,000 x 30 / 4096 would say 74 pages, but the scan
    // reads all 200.
    db::opt::StatsProvider narrowEmp = [](const std::string&) -> std::optional<TableInfo> {
        TableInfo info = emp(true);
        info.stats->width = 30.0;
        return info;
    };
    NodeEstimate scan = db::opt::estimatePlan(PlanNode::scan("Emp"), narrowEmp).at(0);
    CHECK(scan.pages == 200);
    CHECK(scan.width == doctest::Approx(30.0));
}

TEST_CASE("a projection narrows rows and so lowers pages")
{
    // did and age are INTs: 8 bytes, so ceil(10,000 x 8 / 4096) = 20 pages.
    NodeEstimate narrow = root(PlanNode::project({col("Emp", "did"), col("Emp", "age")}, PlanNode::scan("Emp")));
    CHECK(narrow.rows == 10000);
    CHECK(narrow.width == doctest::Approx(8.0));
    CHECK(narrow.pages == 20);

    // name takes what the four INTs leave of 81.92: 65.92 bytes.
    NodeEstimate name = root(PlanNode::project({col("Emp", "name")}, PlanNode::scan("Emp")));
    CHECK(name.width == doctest::Approx(65.92));
}

TEST_CASE("statistics are found through a rename, by column position")
{
    PlanNode plan = selectOn("e.did = 7", PlanNode::rename("e", PlanNode::scan("Emp")));
    CHECK(root(plan).rows == 200);
}

TEST_CASE("never below 1 row")
{
    // 999 lies in no bucket of Emp.did, so the selectivity is 0.
    CHECK(root(selectOn("Emp.did = 999", PlanNode::scan("Emp"))).rows == 1);
    CHECK(root(selectOn("Emp.did = 999", PlanNode::scan("Emp"))).pages == 1);
}

TEST_CASE("cross and non-equality join")
{
    PlanNode scanEmp = PlanNode::scan("Emp");
    PlanNode scanDept = PlanNode::scan("Dept");

    CHECK(root(PlanNode::cross(scanEmp, scanDept)).rows == 500000);
    // Any other join predicate: 0.1 (§M4.2). ceil(500,000 x 0.1).
    CHECK(root(PlanNode::join(db::parseCondition("Emp.age < Dept.did"), scanEmp, scanDept)).rows == 50000);
    // Join width = left width + right width.
    CHECK(root(PlanNode::cross(scanEmp, scanDept)).width == doctest::Approx(81.92 + 81.92));
}

TEST_CASE("after a select, a column has at most as many distinct values as rows")
{
    // age > 60 leaves 2,000 rows, so ndv(eid) drops from 10,000 to 2,000:
    // 2,000 x 50 / max(2,000, 50) = 50. Without the cap: 2,000 x 50 / 10,000 = 10.
    PlanNode left = selectOn("Emp.age > 60", PlanNode::scan("Emp"));
    PlanNode join = PlanNode::join(db::parseCondition("Emp.eid = Dept.did"), left, PlanNode::scan("Dept"));
    CHECK(root(join).rows == 50);
}

TEST_CASE("estimates come in pre-order: root, left subtree, right subtree")
{
    PlanNode join = PlanNode::join(db::parseCondition("Emp.did = Dept.did"),
                                   selectOn("Emp.age > 60", PlanNode::scan("Emp")), PlanNode::scan("Dept"));
    std::vector<NodeEstimate> all = db::opt::estimatePlan(join, provider(true, true));

    REQUIRE(all.size() == 4);
    CHECK(all[0].rows == 2000); // join: 2,000 x 50 / 50
    CHECK(all[1].rows == 2000); // select
    CHECK(all[2].rows == 10000); // scan Emp
    CHECK(all[3].rows == 50); // scan Dept
}

TEST_CASE("rounding: a tolerance keeps 3.0000000000000004 at 3")
{
    double x = (0.1 * 3) * 10; // 3 on paper, 3.0000000000000004 as a double
    CHECK(std::ceil(x) == 4.0); // why plain ceil is not enough
    CHECK(db::opt::ceilRows(x) == 3);
    CHECK(db::opt::ceilRows(2.5) == 3);
    CHECK(db::opt::ceilRows(0.0) == 1);
    CHECK(db::opt::ceilPages(1, 1.0) == 1);
    CHECK(db::opt::ceilPages(4097, 1.0) == 2);
}

TEST_CASE("group_by, distinct, limit, sort and the set operations")
{
    PlanNode scanEmp = PlanNode::scan("Emp");
    Aggregate count{AggregateFunction::Count, std::nullopt, "n"};
    Aggregate avg{AggregateFunction::Avg, col("Emp", "salary"), "a"};

    // One group per did value.
    NodeEstimate byDid = root(PlanNode::groupBy({col("Emp", "did")}, {count, avg}, scanEmp));
    CHECK(byDid.rows == 50);
    CHECK(byDid.width == doctest::Approx(4.0 + 4.0 + 8.0)); // did, count, avg (a REAL)
    // No keys: one group for the whole input.
    CHECK(root(PlanNode::groupBy({}, {count}, scanEmp)).rows == 1);

    // distinct: min(rows, product of every column's ndv).
    PlanNode dids = PlanNode::project({col("Emp", "did")}, scanEmp);
    CHECK(root(PlanNode::distinct(dids)).rows == 50);

    CHECK(root(PlanNode::limit(10, selectOn("Emp.age > 60", scanEmp))).rows == 10);
    CHECK(root(PlanNode::sort({db::SortKey{col("Emp", "age"), true}}, scanEmp)).rows == 10000);

    PlanNode d1 = PlanNode::project({col("Dept", "did")}, PlanNode::scan("Dept"));
    CHECK(root(PlanNode::unionAll(dids, d1)).rows == 10050);
    CHECK(root(PlanNode::unionOf(dids, d1)).rows == 10050);
    CHECK(root(PlanNode::intersect(dids, d1)).rows == 50);
    CHECK(root(PlanNode::except(dids, d1)).rows == 10000);
}

TEST_CASE("no statistics: catalog rows and the documented defaults")
{
    auto rows = [](const PlanNode& plan, bool empAnalyzed, bool deptAnalyzed) {
        return db::opt::estimatePlan(plan, provider(empAnalyzed, deptAnalyzed)).at(0).rows;
    };
    PlanNode scanEmp = PlanNode::scan("Emp");
    PlanNode join = PlanNode::join(db::parseCondition("Emp.did = Dept.did"), scanEmp, PlanNode::scan("Dept"));

    CHECK(rows(scanEmp, false, false) == 10000); // the catalog's exact count
    CHECK(db::opt::estimatePlan(scanEmp, provider(false, false)).at(0).pages == 200);
    CHECK(rows(selectOn("Emp.did = 7", scanEmp), false, false) == 1000); // 0.1
    CHECK(rows(selectOn("Emp.age > 60", scanEmp), false, false) == 3334); // 1/3, rounded up
    CHECK(rows(join, false, false) == 50000); // neither ndv known: 0.1
    CHECK(rows(join, true, false) == 10000); // only Emp.did known: 1/50
    // group_by on a key with no ndv: no reduction.
    CHECK(rows(PlanNode::groupBy({col("Emp", "did")}, {}, scanEmp), false, false) == 10000);
}

TEST_CASE("joinConjunctSelectivity: one factor per conjunct")
{
    TableInfo e = emp(true);
    TableInfo d = dept(true);
    std::vector<db::opt::ColumnFacts> facts;
    for (const ColumnStats& c : e.stats->columns)
    {
        facts.push_back({&c, static_cast<std::int64_t>(c.ndv), 4.0});
    }
    for (const ColumnStats& c : d.stats->columns)
    {
        facts.push_back({&c, static_cast<std::int64_t>(c.ndv), 4.0});
    }

    auto sel = [&](const std::string& text) {
        return db::opt::joinConjunctSelectivity(db::parseCondition(text), e.schema, d.schema, facts);
    };
    CHECK(sel("Emp.did = Dept.did") == doctest::Approx(1.0 / 50));
    CHECK(sel("Dept.did = Emp.eid") == doctest::Approx(1.0 / 10000)); // either way round
    CHECK(sel("Emp.age < Dept.budget") == doctest::Approx(0.1));
    CHECK(sel("Emp.age > 60") == doctest::Approx(0.2)); // one side only: its own selectivity
}

TEST_CASE("unknown table")
{
    CHECK_THROWS_AS(db::opt::estimatePlan(PlanNode::scan("Nope"), provider(true, true)), db::Error);
}
