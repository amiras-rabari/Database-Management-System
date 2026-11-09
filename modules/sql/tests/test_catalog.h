#pragma once

// The catalog every sql test uses: four tables, in memory (the sample
// catalog is not available yet). Emp and Dept have the columns of the
// spec's examples (§M3.5); Proj and Loc give required cases 2 and 15 a third
// and fourth table. `did` and `city` appear in more than one table on
// purpose (cases 2 and 3).

#include <db/sql/table_lookup.h>

#include <db/common/schema.h>
#include <db/common/value.h>

#include <map>
#include <optional>
#include <string>

namespace db::sql::test
{

    // A TableLookup over the four tables. The map is copied into the lambda,
    // so the lookup owns its data. Columns are qualified by the table name,
    // as storage's getTable returns them.
    inline TableLookup testCatalog()
    {
        const std::map<std::string, Schema> tables = {
            {"Emp", Schema({{"Emp", "eid", Type::Int, 0},
                            {"Emp", "name", Type::Varchar, 20},
                            {"Emp", "age", Type::Int, 0},
                            {"Emp", "did", Type::Int, 0},
                            {"Emp", "salary", Type::Int, 0},
                            {"Emp", "mgr", Type::Int, 0},
                            {"Emp", "city", Type::Varchar, 20}})},
            {"Dept", Schema({{"Dept", "did", Type::Int, 0},
                             {"Dept", "title", Type::Varchar, 20},
                             {"Dept", "budget", Type::Int, 0},
                             {"Dept", "city", Type::Varchar, 20}})},
            {"Proj", Schema({{"Proj", "pid", Type::Int, 0},
                             {"Proj", "did", Type::Int, 0},
                             {"Proj", "pname", Type::Varchar, 20},
                             {"Proj", "lid", Type::Int, 0}})},
            {"Loc", Schema({{"Loc", "lid", Type::Int, 0}, {"Loc", "region", Type::Varchar, 20}})},
        };
        return [tables](const std::string& name) -> std::optional<Schema>
        {
            const auto found = tables.find(name);
            if (found == tables.end())
            {
                return std::nullopt;
            }
            return found->second;
        };
    }

} // namespace db::sql::test
