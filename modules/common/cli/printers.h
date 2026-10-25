#pragma once

/// @file
/// Bonus 1 style text views of schemas, result tables and plan trees (CLI only).

#include "db/common/plan.h"
#include "db/common/schema.h"

#include <ostream>
#include <string>
#include <vector>

namespace ra
{
    /// The columns with their types: `Emp.EID:VARCHAR(2), Emp.Age:INT`.
    std::string schemaText(const db::Schema& schema);

    /// Prints `name (col, ...) =` followed by one row per line (each value
    /// followed by a space) and an empty line, as Bonus 1 did.
    void printTable(std::ostream& out, const std::string& name, const db::Schema& schema,
                    const std::vector<db::Tuple>& rows);

    /// Prints the plan as an indented diagram, one node per line:
    /// ```
    /// join [Emp.DID = Dept.DID]
    /// |---- scan Emp
    /// |____ scan Dept
    /// ```
    void printPlan(std::ostream& out, const db::PlanNode& plan);
}
