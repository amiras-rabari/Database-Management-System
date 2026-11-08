#pragma once

// group_by (SPEC §M2.3, chunk 10): the aggregate rules shared by both modes,
// and the sort-mode operator. The hash mode is in operators/hash_group.h.
// Private to exec; plans reach them through buildExecutablePlan.
//
// The idea both modes share is a "state row": a group's key columns, then
// what each aggregate has gathered so far:
//   count    -> INT  number of rows
//   sum      -> REAL total (a double: exact up to 2^53, far beyond any sum
//               that still fits the INT output)
//   avg      -> REAL total, then INT number of rows
//   min, max -> the smallest / largest value so far, of the column's type
// One input row becomes a state row of one row (toStateRow); two state rows
// of the same group combine into one (mergeStateRow); at the end the state
// row becomes the output row (finishStateRow). Because a state row is an
// ordinary tuple, hash mode can keep it in its table, spill it and
// partition it with chunk 09's pieces.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/external_sort.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"

namespace db::exec
{
    /// What a group_by (or a hash distinct) computes, worked out once from
    /// the child's schema while the plan is built.
    struct Grouping
    {
        /// Where each key column is in the child's rows. The state and
        /// output rows start with these columns, in this order.
        std::vector<std::size_t> keyColumns;
        /// Each aggregate's function, the child position of its column
        /// (unused for count), and its output name (for messages).
        std::vector<AggregateFunction> functions;
        std::vector<std::size_t> inputColumns;
        std::vector<std::string> names;
        /// The columns of a state row and of an output row.
        Schema stateSchema;
        Schema outputSchema;
        /// True when some MIN or MAX is of a VARCHAR: a state row can then
        /// grow, so a table must count each entry at its largest size.
        bool stateCanGrow = false;
    };

    /// The grouping of a group_by node. Checks, before any page is read:
    /// unknown or ambiguous key/aggregate column -> Error(Name)/Ambiguity;
    /// sum or avg of a column that is not INT -> Error(Type) (§M2.3 "SUM and
    /// AVG require INT"); two outputs with the same name -> Error(Schema).
    /// Aggregate columns are named by aggregateOutputName, with no qualifier.
    Grouping makeGrouping(const Schema& child, const std::vector<ColumnRef>& keys,
                          const std::vector<Aggregate>& aggregates);

    /// distinct as a grouping: every column is a key, no aggregates, so the
    /// state and output rows are the input rows.
    Grouping distinctGrouping(const Schema& child);

    /// One input row as the state row of a group of one row.
    Tuple toStateRow(const Grouping& grouping, const Tuple& input);

    /// Adds the state row `from` into `into` (same key). Each MIN/MAX check
    /// adds 1 to `comparisons`.
    void mergeStateRow(const Grouping& grouping, Tuple& into, const Tuple& from, std::uint64_t& comparisons);

    /// The output row of a finished group: avg = total / count as a REAL.
    /// A sum outside the INT range -> Error(Type), e.g. "sum_R_salary =
    /// 3000000000 does not fit in INT." (DECISIONS 2026-11-07).
    Tuple finishStateRow(const Grouping& grouping, const Tuple& state);

    /// The one row of a group_by with no keys whose input is empty (§M2.3):
    /// every count is 0. Any other aggregate is undefined there, so it
    /// throws Error(Aggregate) naming it, e.g. "avg_R_salary is undefined:
    /// the input is empty and there are no grouping columns."
    Tuple emptyInputRow(const Grouping& grouping);

    /// group_by in sort mode.
    /// - With keys: open() sorts the input on the keys (an ExternalSorter,
    ///   frames as SortOperator). After the sort the rows of a group are
    ///   next to each other, so next() adds rows into one state row while
    ///   the key stays the same, and returns the group when it changes.
    /// - With no keys: no sort. open() reads the whole input into one state
    ///   row; next() returns that one row (or the empty-input row).
    /// Memory: the sorter's frames plus one state row and one input row.
    /// Comparisons: the sort's, + 1 per key check, + 1 per MIN/MAX check.
    class SortGroupByOperator : public Operator
    {
    public:
        /// `grouping` must come from makeGrouping on the child's schema.
        SortGroupByOperator(ExecContext& ctx, std::unique_ptr<Operator> child, Grouping grouping,
                            const FrameShare& share);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        Grouping grouping_;
        FrameShare share_;
        /// Exists only between open and close, and only with keys.
        std::optional<ExternalSorter> sorter_;
        /// The group being gathered, if there is one left to return.
        Tuple current_;
        bool hasCurrent_ = false;
        /// No keys and an empty input: next() returns emptyInputRow once.
        bool returnEmptyRow_ = false;
    };
} // namespace db::exec
