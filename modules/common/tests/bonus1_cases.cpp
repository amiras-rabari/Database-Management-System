// The Bonus 1 QueryTester cases as assertions.
//
// Expected values come from tests/baseline/ (output of the unchanged Bonus 1
// code). Cases 1-25 are the official suite; 11b and 23b are extras.
// Every change to an expected value is recorded in STATUS.md with its reason.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "bonus1_adapter.h"

namespace {

using bonus1::CaseResult;
using Rows = std::vector<std::vector<std::string>>;

const std::string kEmpDeptDb = R"(
Emp (EID, Name, Age, DID, MgrID) = {
  E1, Alice, 30, D1, E3
  E2, Bob, 25, D2, E3
  E3, Carol, 45, D1, E5
  E2, Bob, 25, D2, E3
}
Dept (DID, DName, Budget) = {
  D1, Sales, 100
  D2, Eng, 2000
}
)";
const std::string kSimpleRDb = "R (a, b) = { 1, 2 \n 3, 3 }";
const std::string kSetMathDb = "A (k) = { 1 } \n B (k) = { 1 } \n C (k) = { 1 }";
const std::string kMismatchDb = "R (a, b) = { 1, 2 } \n S (c, d, e) = { 1, 2, 3 }";

struct Expected {
    std::vector<std::string> columns;
    Rows rows;
    std::uint64_t joinComparisons;
    std::uint64_t selectTuples;
};

std::string join(const std::vector<std::string>& values, const char* separator) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += separator;
        out += value;
    }
    return out;
}

// Rows are a bag: sorting both sides makes the comparison order-independent
// while still counting duplicates.
std::string bagText(Rows rows) {
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (const auto& row : rows) out += join(row, " ") + "\n";
    return out;
}

void checkTree(const std::string& query, const std::string& expectedTree) {
    const CaseResult result = bonus1::parseCase(query);
    INFO("query: ", query, "\nerror: ", result.message);
    REQUIRE(result.ok);
    CHECK(result.tree == expectedTree);
}

void checkParseError(const std::string& query, const std::string& expectedCategory) {
    const CaseResult result = bonus1::parseCase(query);
    INFO("query: ", query, "\nerror: ", result.message);
    REQUIRE_FALSE(result.ok);
    CHECK(result.category == expectedCategory);
}

void checkRun(const std::string& db, const std::string& query, const Expected& expected) {
    const CaseResult result = bonus1::runCase(db, query);
    INFO("query: ", query, "\nerror: ", result.message);
    REQUIRE(result.ok);
    CHECK(join(result.columns, ", ") == join(expected.columns, ", "));
    CHECK(bagText(result.rows) == bagText(expected.rows));
    CHECK(result.rows.size() == expected.rows.size());
    CHECK(result.joinComparisons == expected.joinComparisons);
    CHECK(result.selectTuples == expected.selectTuples);
}

void checkRunError(const std::string& db, const std::string& query, const std::string& expectedCategory) {
    const CaseResult result = bonus1::runCase(db, query);
    INFO("query: ", query, "\nerror: ", result.message);
    REQUIRE_FALSE(result.ok);
    CHECK(result.category == expectedCategory);
}

const std::vector<std::string> kEmpColumns = {"Emp.EID", "Emp.Name", "Emp.Age", "Emp.DID", "Emp.MgrID"};

}  // namespace

TEST_SUITE("bonus1 official") {
    // 7.1 Tokenizer
    TEST_CASE("01 no whitespace") { checkTree("select[x1=3](R)", "select[(x1 = 3)](scan(R))"); }
    TEST_CASE("02 whitespace padded") { checkTree("select[ x1 = 3 ](R)", "select[(x1 = 3)](scan(R))"); }
    TEST_CASE("03 >= operator") { checkTree("select[Age>=30](R)", "select[(Age >= 30)](scan(R))"); }
    TEST_CASE("04 > and - operators") { checkTree("select[Age>-30](R)", "select[(Age > -30)](scan(R))"); }
    TEST_CASE("05 parenthesis inside string") {
        checkTree("select[Name='Bob)'](R)", "select[(Name = 'Bob)')](scan(R))");
    }
    TEST_CASE("06 comma inside string") { checkTree("select[Name='a,b'](R)", "select[(Name = 'a,b')](scan(R))"); }
    TEST_CASE("07 doubled quote inside string") {
        checkTree("select[Name='O''Brien'](R)", "select[(Name = 'O''Brien')](scan(R))");
    }
    TEST_CASE("08 attribute spelled like keyword") {
        checkTree("select[union=3](R)", "select[(union = 3)](scan(R))");
    }
    TEST_CASE("09 unclosed string is a lexical error") { checkParseError("select[Name='Bob](R)", "lexical"); }

    // 7.2 Grammar and precedence
    TEST_CASE("10 A union B minus C") {
        checkTree("A union B minus C", "except(union(scan(A), scan(B)), scan(C))");
    }
    TEST_CASE("11 A minus B minus C is left associative") {
        checkTree("A minus B minus C", "except(except(scan(A), scan(B)), scan(C))");
    }
    TEST_CASE("12 precedence not > and > or") {
        checkTree("select[not (a=1 and b=2) or c>3](R)",
                  "select[((not ((a = 1) and (b = 2))) or (c > 3))](scan(R))");
    }
    TEST_CASE("13 precedence and before or") {
        checkTree("select[a=1 and b=2 or c=3](R)", "select[(((a = 1) and (b = 2)) or (c = 3))](scan(R))");
    }
    TEST_CASE("14 deep nesting") {
        checkTree("project[Name](select[Age>30](select[DID='D1'](Employees)))",
                  "project[Name](select[(Age > 30)](select[(DID = 'D1')](scan(Employees))))");
    }
    TEST_CASE("15 explicit parentheses") {
        checkTree("(A union B) minus (C intersect D)",
                  "except(union(scan(A), scan(B)), intersect(scan(C), scan(D)))");
    }
    TEST_CASE("16 missing parenthesis is a syntax error") { checkParseError("select[Age>30](R", "syntax"); }
    TEST_CASE("17 empty attribute list is a syntax error") { checkParseError("project[](R)", "syntax"); }

    // 7.3 Semantics
    TEST_CASE("18 compare two columns") {
        checkRun(kSimpleRDb, "select[a=b](R)", {{"R.a", "R.b"}, {{"3", "3"}}, 0, 2});
    }
    // Bags (Phase 0 point 6): the duplicate E2 row is no longer removed at load, so it joins twice and 4 x 2 pairs.
    TEST_CASE("19 qualified names / join") {
        checkRun(kEmpDeptDb, "Emp join[Emp.DID=Dept.DID] Dept",
                 {{"Emp.EID", "Emp.Name", "Emp.Age", "Emp.DID", "Emp.MgrID", "Dept.DID", "Dept.DName", "Dept.Budget"},
                  {{"E1", "Alice", "30", "D1", "E3", "D1", "Sales", "100"},
                   {"E2", "Bob", "25", "D2", "E3", "D2", "Eng", "2000"},
                   {"E3", "Carol", "45", "D1", "E5", "D1", "Sales", "100"},
                   {"E2", "Bob", "25", "D2", "E3", "D2", "Eng", "2000"}},
                  8, 0});
    }
    // Bags (Phase 0 point 6): the duplicate E2 row is kept on both sides, so Carol manages it twice and 4 x 4 pairs.
    TEST_CASE("20 self join with rename") {
        checkRun(kEmpDeptDb, "rename[E2](Emp) join[Emp.MgrID=E2.EID] Emp",
                 {{"E2.EID", "E2.Name", "E2.Age", "E2.DID", "E2.MgrID", "Emp.EID", "Emp.Name", "Emp.Age", "Emp.DID",
                   "Emp.MgrID"},
                  {{"E3", "Carol", "45", "D1", "E5", "E1", "Alice", "30", "D1", "E3"},
                   {"E3", "Carol", "45", "D1", "E5", "E2", "Bob", "25", "D2", "E3"},
                   {"E3", "Carol", "45", "D1", "E5", "E2", "Bob", "25", "D2", "E3"}},
                  16, 0});
    }
    TEST_CASE("21 union of incompatible schemas is a schema error") {
        checkRunError(kMismatchDb, "R union S", "schema");
    }
    TEST_CASE("22 comparing number to string is a type error") {
        checkRunError(kEmpDeptDb, "select[Age>'30'](Emp)", "type");
    }
    // Bags (Phase 0 point 6): project keeps one row per Emp row (4, duplicates kept); 23b covers the old dedup.
    TEST_CASE("23 projection keeps duplicates (bags; was: projection duplicate removal)") {
        checkRun(kEmpDeptDb, "project[DID](Emp)", {{"Emp.DID"}, {{"D1"}, {"D2"}, {"D1"}, {"D2"}}, 0, 0});
    }
    TEST_CASE("24 duplicate projection columns is a schema error") {
        checkRunError(kEmpDeptDb, "project[Name, Name](Emp)", "schema");
    }
    // Bags (Phase 0 point 6): the duplicate E2 row is no longer removed at load, so select examines 4 tuples.
    TEST_CASE("25 query returning no tuples") {
        checkRun(kEmpDeptDb, "select[Age>999](Emp)", {kEmpColumns, {}, 0, 4});
    }
}

TEST_SUITE("bonus1 extra") {
    TEST_CASE("11b (A-B)-C on data") { checkRun(kSetMathDb, "A minus B minus C", {{"A.k"}, {}, 0, 0}); }

    // Bags (Phase 0 point 6): case 23's original set result, now asked for explicitly with distinct.
    TEST_CASE("23b distinct restores duplicate removal") {
        checkRun(kEmpDeptDb, "distinct(project[DID](Emp))", {{"Emp.DID"}, {{"D1"}, {"D2"}}, 0, 0});
    }
}
