#pragma once

/// @file
/// The relational operators of the reference engine. Each one takes its
/// input source(s) and returns a new source; buildOperators turns a whole
/// plan tree into one source.
///
/// Results are bags: select, project, rename, cross and join keep duplicate
/// tuples. Only distinct and the set operations (union, intersect, except)
/// remove them.
///
/// This is a reference engine, not M2's executor: operators may hold tuples
/// in memory (distinct and the set operations keep every tuple they have
/// seen) and are NOT budget-aware. The B-frame rule applies to M2's physical
/// operators.
///
/// Every operator computes its output schema and binds its condition when it
/// is constructed, so every Name, Ambiguity, Type and Schema error is thrown
/// before a single tuple is read. Reading never throws db::Error.
///
/// rewind() on an operator rewinds its inputs and throws away any state it
/// built while reading, so a rewound operator behaves exactly like a fresh
/// one. Reading a subtree again therefore counts again: a select below the
/// right input of a join is re-read (and re-counted) once per left tuple.
/// Bonus 1 evaluated every subtree exactly once instead; the two agree
/// whenever every select sits on a path of left inputs only.

#include <functional>
#include <string>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/tuple_source.h"

namespace db
{

    /// The tuples of `input` for which `condition` holds, in input order.
    /// Adds 1 to `counters.selectTuples` for every input tuple examined.
    /// Throws what bindCondition throws (Name, Ambiguity, Type).
    /// `counters` must outlive the returned source.
    TupleSourcePtr select(TupleSourcePtr input, const Condition& condition, OperatorCounters& counters);

    /// The `columns` of every `input` tuple, in that order, one output tuple
    /// per input tuple: duplicates are kept (use distinct to remove them).
    /// Throws what projectSchema throws (Name, Ambiguity, Schema for a column
    /// named twice).
    TupleSourcePtr project(TupleSourcePtr input, const std::vector<ColumnRef>& columns);

    /// The tuples of `input`, each once: a tuple equal to one already
    /// produced is dropped, so the first occurrence is kept, in input order.
    /// The schema is the input's. Counts nothing and throws no db::Error.
    TupleSourcePtr distinct(TupleSourcePtr input);

    /// `input` with every column qualified by `alias`; tuples are unchanged.
    /// Throws what requalify throws (Schema).
    TupleSourcePtr rename(TupleSourcePtr input, const std::string& alias);

    /// Every pair of a `left` and a `right` tuple, as one tuple (left values
    /// first), in nested-loop order: for each left tuple, every right tuple.
    /// The right input is rewound before each left tuple. Counts nothing.
    /// Throws what concatSchemas throws (Schema).
    TupleSourcePtr cross(TupleSourcePtr left, TupleSourcePtr right);

    /// The pairs of `cross` for which `condition` holds, by nested loop: for
    /// each left tuple the right input is rewound and read to the end. Adds 1
    /// to `counters.joinComparisons` for every pair examined, so a full read
    /// counts |left| x |right|. Throws what concatSchemas throws (Schema, e.g.
    /// a self join without a rename), then what bindJoinCondition throws.
    /// `counters` must outlive the returned source.
    TupleSourcePtr join(TupleSourcePtr left, TupleSourcePtr right, const Condition& condition,
                        OperatorCounters& counters);

    /// The tuples of `left`, then the tuples of `right`, each tuple once (the
    /// first occurrence is kept). Result schema: the left schema. Throws what
    /// checkUnionCompatible throws (Schema).
    TupleSourcePtr unionOf(TupleSourcePtr left, TupleSourcePtr right);

    /// The tuples of `left` that also occur in `right`, each once, in left
    /// order. Reads all of `right` at the first next(). Result schema: the
    /// left schema. Throws what checkUnionCompatible throws (Schema).
    TupleSourcePtr intersect(TupleSourcePtr left, TupleSourcePtr right);

    /// The tuples of `left` that do not occur in `right`, each once, in left
    /// order. Reads all of `right` at the first next(). Result schema: the
    /// left schema. Throws what checkUnionCompatible throws (Schema).
    TupleSourcePtr except(TupleSourcePtr left, TupleSourcePtr right);

    /// Opens the stored table a Scan names. It must return a source whose
    /// columns are qualified by the table name, or throw Error(Name)
    /// "Unknown relation 'X'." for a name it does not know.
    using OpenTable = std::function<TupleSourcePtr(const std::string& table)>;

    /// Builds the operators for `plan`, children before their parent and
    /// left before right (so the first error found is the one outputSchema
    /// finds), calling `openTable` once for each Scan. Throws the errors of
    /// the operators above and of `openTable`; std::logic_error for a
    /// malformed node (wrong number of children, missing condition) or a
    /// null source from `openTable`. Plans up to kMaxNestingDepth levels
    /// deep build and run; deeper ones may overflow the stack. `counters`
    /// must outlive the returned source.
    TupleSourcePtr buildOperators(const PlanNode& plan, const OpenTable& openTable, OperatorCounters& counters);

} // namespace db
