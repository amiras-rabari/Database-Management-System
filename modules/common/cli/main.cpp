#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "db/common/error.h"
#include "db/common/operators.h"
#include "db/common/plan_json.h"
#include "printers.h"
#include "program.h"
#include "vector_source.h"

int main(int argc, char* argv[])
{
    bool treeOnlyMode = false;
    bool showJson = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--tree")
        {
            treeOnlyMode = true;
        }
        else if (arg == "--json")
        {
            showJson = true;
        }
    }

    std::cout << "======================================================================\n";
    std::cout << " Mode: " << (treeOnlyMode ? "Tree-Only Mode" : "Normal Mode (Database + Query)") << "\n";
    std::cout << "======================================================================\n";
    std::cout << (treeOnlyMode ? "Enter Query Only" :"Enter your input (Relations + Query). You can use multiple lines.")<<"\n";
    std::cout << "Enter and then Type'END' on newline or put a ';' at the end of query when finished either should work:\n";
    std::cout << "----------------------------------------------------------------------\n\n";

    std::string full_input;
    std::string line;

    while (std::getline(std::cin, line))
    {
        // Trim trailing spaces/carriage returns
        std::string trimmed = line;
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

        // Check for keyword "END" / "end" on a new line
        if (trimmed == "END" || trimmed == "end")
        {
            break;
        }

        full_input += line + "\n";

        // Check if the current line ends with a semicolon ';'
        if (!trimmed.empty() && trimmed.back() == ';')
        {
            // Remove the trailing semicolon if your grammar doesn't expect it
            full_input.pop_back(); // removes '\n'
            if (!full_input.empty() && full_input.back() == ';')
            {
                full_input.pop_back();
            }
            full_input += "\n";
            break;
        }
    }

    if (full_input.empty())
    {
        std::cerr << "Error: No input provided.\n";
        return 1;
    }

    const char* const separator =
        "-----------------------------------------------------------------------------------------------------------------\n";

    // The plan as the JSON the other modules exchange (--json).
    const auto printPlanJson = [&](const db::PlanNode& plan)
    {
        if (showJson)
        {
            std::cout << separator;
            std::cout << "Plan JSON\n";
            std::cout << "-------------\n";
            std::cout << db::toJsonText(plan) << "\n";
        }
    };

    try
    {
        ra::Program program = ra::parseProgram(full_input, treeOnlyMode);

        // --- TREE ONLY MODE ---
        if (treeOnlyMode)
        {
            std::cout << separator;
            std::cout << "Plan tree\n";
            ra::printPlan(std::cout, program.query);
            printPlanJson(program.query);
            return 0;
        }

        // --- NORMAL EXECUTION MODE ---
        std::cout << "[OK] Semantic relation validation passed.\n";

        std::map<std::string, const ra::Relation*> relationsByName;
        for (const ra::Relation& relation : program.relations)
        {
            relationsByName[relation.name] = &relation;
        }
        const auto openTable = [&relationsByName](const std::string& name) -> db::TupleSourcePtr
        {
            const auto found = relationsByName.find(name);
            if (found == relationsByName.end())
            {
                throw db::Error(db::ErrorCategory::Name, "Unknown relation '" + name + "'.");
            }
            return std::make_unique<ra::VectorSource>(found->second->schema, found->second->rows);
        };

        // Building the operators checks every name and type of the query.
        db::OperatorCounters counters;
        db::TupleSourcePtr result = db::buildOperators(program.query, openTable, counters);

        std::cout << "[OK] Semantic query validation passed.\n";
        std::cout << separator;
        std::cout << "Output schema: Result (" << ra::schemaText(result->schema()) << ")\n";

        std::vector<db::Tuple> rows;
        db::Tuple tuple;
        while (result->next(tuple))
        {
            rows.push_back(tuple);
        }

        std::cout << "\nResult of the Query\n";
        std::cout << "------------------------\n";
        ra::printTable(std::cout, "Result", result->schema(), rows);
        std::cout << "JOIN Comparisons     : " << counters.joinComparisons << "\n";
        std::cout << "SELECT Comparisons   : " << counters.selectTuples << "\n";
        std::cout << "Output Tuples        : " << rows.size() << "\n";
        std::cout << separator;
        std::cout << "Plan tree\n";
        std::cout << "-------------\n";
        ra::printPlan(std::cout, program.query);
        printPlanJson(program.query);

        std::cout << separator;
        std::cout << "Database Schema\n";
        std::cout << "----------------\n";
        for (const ra::Relation& relation : program.relations)
        {
            std::cout << relation.name << ": " << ra::schemaText(relation.schema) << "\n";
            ra::printTable(std::cout, relation.name, relation.schema, relation.rows);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n" << e.what() << "\n";
        return 1;
    }

    return 0;
}
