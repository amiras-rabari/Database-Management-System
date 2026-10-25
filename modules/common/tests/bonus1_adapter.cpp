// Adapter between the Bonus 1 cases and the engine under test.
// Parse-only cases run the library RA parser (db::parseRaQuery); execution
// cases load the relations with the CLI loader and run the plan through the
// library operators (db::buildOperators) over in-memory VectorSources.

#include "bonus1_adapter.h"

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "db/common/error.h"
#include "db/common/operators.h"
#include "db/common/plan.h"
#include "db/common/ra_parser.h"
#include "db/common/tuple_source.h"
#include "program.h"
#include "vector_source.h"

namespace bonus1 {
namespace {

CaseResult failure(std::string category, const std::exception& error) {
    CaseResult result;
    result.category = std::move(category);
    result.message = error.what();
    return result;
}

CaseResult failure(const db::Error& error) {
    return failure(std::string(db::categoryName(error.category())), error);
}

// The library reports user mistakes only as db::Error; anything else is a
// bug, and this category never equals an expected one.
CaseResult unexpected(const std::exception& error) {
    return failure(std::string("unexpected: ") + error.what(), error);
}

}  // namespace

CaseResult parseCase(const std::string& queryText) {
    try {
        const db::PlanNode plan = db::parseRaQuery(queryText);
        CaseResult result;
        result.ok = true;
        result.tree = db::toString(plan, db::ConditionStyle::Explicit);
        return result;
    } catch (const db::Error& error) {
        return failure(error);
    } catch (const std::exception& error) {
        return unexpected(error);
    }
}

CaseResult runCase(const std::string& dbText, const std::string& queryText) {
    try {
        const ra::Program program = ra::parseProgram(dbText + "\n" + queryText, false);

        const auto openTable = [&program](const std::string& name) -> db::TupleSourcePtr {
            for (const ra::Relation& relation : program.relations) {
                if (relation.name == name) return std::make_unique<ra::VectorSource>(relation.schema, relation.rows);
            }
            throw db::Error(db::ErrorCategory::Name, "Unknown relation '" + name + "'.");
        };
        db::OperatorCounters counters;
        const db::TupleSourcePtr source = db::buildOperators(program.query, openTable, counters);

        CaseResult result;
        result.ok = true;
        for (const db::Column& column : source->schema().columns()) result.columns.push_back(column.qualifiedName());
        db::Tuple tuple;
        while (source->next(tuple)) {
            std::vector<std::string> row;
            for (const db::Value& value : tuple) row.push_back(value.toText());
            result.rows.push_back(std::move(row));
        }
        result.joinComparisons = counters.joinComparisons;
        result.selectTuples = counters.selectTuples;
        return result;
    } catch (const db::Error& error) {
        return failure(error);
    } catch (const std::exception& error) {
        return unexpected(error);
    }
}

}  // namespace bonus1
