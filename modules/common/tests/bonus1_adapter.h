#pragma once

/// @file
/// The single seam between the Bonus 1 test cases and the engine under test.
///
/// bonus1_cases.cpp asserts only on CaseResult. When the engine's API changes
/// during the Phase 0 refactor, only bonus1_adapter.cpp is rewritten; the
/// expected values in the cases stay fixed, so a green suite means the old
/// behaviour survived.

#include <cstdint>
#include <string>
#include <vector>

namespace bonus1 {

/// Outcome of one Bonus 1 case, reduced to the parts the tests compare.
struct CaseResult {
    /// True when the case completed without an error.
    bool ok = false;
    /// Error category (`lexical`, `syntax`, `name`, `ambiguity`, `type`,
    /// `schema`) when `ok` is false; empty otherwise.
    std::string category;
    /// Full error text, shown only in failure output; never asserted.
    std::string message;

    /// Parse-only cases: the query tree in explicit style, e.g.
    /// `except(except(scan(A), scan(B)), scan(C))`.
    std::string tree;

    /// Execution cases: qualified output column names, in order.
    std::vector<std::string> columns;
    /// Execution cases: each row as the text of its values.
    std::vector<std::vector<std::string>> rows;
    /// Execution cases: Bonus 1 counter, one per tuple pair examined by a join.
    std::uint64_t joinComparisons = 0;
    /// Execution cases: Bonus 1 counter, one per tuple examined by a select.
    std::uint64_t selectTuples = 0;
};

/// Parses `query` without any relations (Bonus 1 "tree only" mode).
CaseResult parseCase(const std::string& query);

/// Loads the relations in `dbText` (Bonus 1 relation-text format), then
/// validates and executes `query` against them.
CaseResult runCase(const std::string& dbText, const std::string& query);

}  // namespace bonus1
