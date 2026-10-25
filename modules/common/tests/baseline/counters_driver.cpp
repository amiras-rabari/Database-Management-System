// Baseline-only driver: runs the execution cases of QueryTester against the
// unchanged Bonus 1 sources and prints the two Executioner counters.
#include <iostream>
#include <string>
#include <exception>

#include "Tokenizer.h"
#include "Parser.h"
#include "SemanticValidator.h"
#include "Executioner.h"

static void run(const std::string& name, const std::string& db, const std::string& q) {
    std::cout << name << ": ";
    try {
        query::Tokenizer lexer(db + "\n" + q);
        auto tokens = lexer.tokenizeAll();
        query::Parser parser{tokens};
        auto program = parser.parseProgram(false);
        query::SemanticValidator validator;
        query::Database database;
        if (program.database.has_value()) database = validator.validateDatabase(program.database.value());
        validator.validateQuery(*program.query);
        query::Executioner ex(database);
        query::Table result = ex.execute(*program.query);
        std::cout << "rows=" << result.rows.size()
                  << " joinComparisons=" << ex.getJoinComparisonCount()
                  << " selectTuples=" << ex.getSelectTupleCount() << "\n";
    } catch (const std::exception& e) {
        std::cout << "error (not executed; no counters)\n";
    }
}

int main() {
    std::string empDeptDB = R"(
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
    std::string simpleR = "R (a, b) = { 1, 2 \n 3, 3 }";
    std::string setMathDB = "A (k) = { 1 } \n B (k) = { 1 } \n C (k) = { 1 }";
    std::string mismatchDB = "R (a, b) = { 1, 2 } \n S (c, d, e) = { 1, 2, 3 }";

    run("18", simpleR, "select[a=b](R)");
    run("19", empDeptDB, "Emp join[Emp.DID=Dept.DID] Dept");
    run("20", empDeptDB, "rename[E2](Emp) join[Emp.MgrID=E2.EID] Emp");
    run("21", mismatchDB, "R union S");
    run("22", empDeptDB, "select[Age>'30'](Emp)");
    run("23", empDeptDB, "project[DID](Emp)");
    run("24", empDeptDB, "project[Name, Name](Emp)");
    run("25", empDeptDB, "select[Age>999](Emp)");
    run("11b", setMathDB, "A minus B minus C");
    return 0;
}
