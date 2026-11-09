#pragma once

/// @file
/// The SQL front end's main entry point: script text in, one checked
/// statement per SQL statement out. sql never executes anything: CREATE TABLE
/// and INSERT come back as plain values, and the shell carries them out
/// through storage (only storage touches files).

#include <db/sql/table_lookup.h>

#include <db/common/plan.h>
#include <db/common/schema.h>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace db::sql
{

    /// A checked `CREATE TABLE`. The shell passes both fields to storage's
    /// `Catalog::createTable`, which stores the columns bare.
    struct CreateTablePlan
    {
        std::string table;
        /// Columns qualified by the table name (`T.a`), in the order written,
        /// the same form TableLookup returns.
        Schema schema;
    };

    /// A checked `INSERT`: one row with a value for every column, in schema
    /// order (there are no NULLs). The shell inserts it with
    /// `HeapFile::insert` and updates the catalog counts.
    struct InsertPlan
    {
        std::string table;
        Tuple tuple;
    };

    /// One compiled statement: CREATE, INSERT, or a query's logical plan.
    using CompiledStatement = std::variant<CreateTablePlan, InsertPlan, PlanNode>;

    /// Parses `text` (any number of statements, as parseSqlScript) and
    /// compiles each one in order. The first error stops everything and
    /// nothing is returned (§M3.6).
    /// - A query: translateQuery's plan and errors.
    /// - CREATE TABLE: the schema must be legal (common's Schema rule):
    ///   Error(Schema) for VARCHAR(n) outside 1..255 or a column written
    ///   twice, at the table name. "Table already exists" is not checked
    ///   here; storage reports it when the shell creates the table.
    /// - INSERT: Error(Name) "Unknown table 'X'." at the table name;
    ///   Error(Schema) when the number of values differs from the number of
    ///   columns, at the table name; Error(Type) when a literal's type
    ///   differs from its column's (a string is never an INT), and
    ///   Error(Schema) for a string longer than VARCHAR(n), both at the
    ///   literal.
    /// Only `lookup` is consulted: a table created earlier in the same text
    /// is still unknown to a later statement. The shell compiles and runs
    /// one statement at a time, so there each CREATE is seen by the next.
    std::vector<CompiledStatement> compileSql(std::string_view text, const TableLookup& lookup);

} // namespace db::sql
