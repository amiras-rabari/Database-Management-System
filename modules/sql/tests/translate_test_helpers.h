#pragma once

// Helpers shared by the resolution, checker and translation tests: compile
// one SQL query against the test catalog, or get the error it gives.

#include "test_catalog.h"

#include <db/sql/parser.h>
#include <db/sql/translate.h>

#include <db/common/error.h>
#include <db/common/plan.h>
#include <db/common/plan_json.h>
#include <db/common/schema.h>

#include <doctest/doctest.h>

#include <string>

namespace db::sql::test
{

    // The logical plan of one query. Every plan is also checked twice, so it
    // is known to load (the M3 half of checklist "plans load and run"):
    // - common's outputSchema binds every name and condition in it;
    // - its JSON, read back by common's logical reader, gives the same tree
    //   (valid field names, 3-element aggregates, condition strings that
    //   common's parser reads, as exec's will).
    inline PlanNode compile(const std::string& sql)
    {
        INFO("sql: ", sql);
        PlanNode plan = translateQuery(parseSqlQuery(sql), testCatalog());
        outputSchema(plan, testCatalog());
        // A named bool: doctest cannot print a PlanNode in CHECK(a == b).
        const bool roundTrips = fromJsonText(toJsonText(plan)) == plan;
        CHECK(roundTrips);
        return plan;
    }

    // The plan on one line (common's toString), easy to compare in a test.
    inline std::string planText(const std::string& sql)
    {
        return toString(compile(sql));
    }

    // The error a query gives. Fails the test if it compiles.
    inline Error compileError(const std::string& sql)
    {
        try
        {
            translateQuery(parseSqlQuery(sql), testCatalog());
        }
        catch (const Error& error)
        {
            return error;
        }
        FAIL("compiled without an error: " << sql);
        return Error(ErrorCategory::Schema, "");
    }

    // 1-based column of the LAST occurrence of `word` in `sql` (the queries
    // here are one line), so a test can say where an error must point.
    inline int columnOf(const std::string& sql, const std::string& word)
    {
        return static_cast<int>(sql.rfind(word)) + 1;
    }

    // Checks an error's category, message and position (line 1).
    inline void checkError(const std::string& sql, ErrorCategory category, const std::string& message, int column)
    {
        INFO("sql: ", sql);
        const Error error = compileError(sql);
        CHECK(error.category() == category);
        CHECK(error.message() == message);
        CHECK(error.line() == 1);
        CHECK(error.column() == column);
    }

} // namespace db::sql::test
