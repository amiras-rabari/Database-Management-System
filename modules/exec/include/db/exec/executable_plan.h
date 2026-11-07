#pragma once

/// @file
/// Turns a physical plan (a PlanNode tree, chunk 02) into a tree of
/// operators and runs it. ExecutablePlan is the one object the exec CLI,
/// the optimizer and the shell use to run a plan.
///
/// Every check happens while the plan is built, before any page is read
/// (chunk 05 Q1 (a), SPEC §M2.2): B too small, unknown table, unknown or
/// ambiguous column, INT against VARCHAR, bad schemas. For the user, build
/// and open are one step, so "rejected while building" is "rejected at
/// open, before any execution work begins".

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/exec/counters.h"
#include "db/exec/exec_context.h"
#include "db/exec/operator.h"

namespace db::exec
{
    /// One line of the trace (SPEC §M2.5): one plan node and its counters.
    struct TraceRow
    {
        /// Distance from the root: 0 for the root, 1 for its children, ...
        std::size_t depth = 0;
        /// The op name as plan JSON spells it, plus the table for a seqscan:
        /// "seqscan R", "filter", "hash_join".
        std::string label;
        /// The work this node did itself.
        ExecCounters counters;
        /// This node plus every node below it. The text trace prints these
        /// page reads/writes (D1 (b)).
        ExecCounters subtree;

        friend bool operator==(const TraceRow&, const TraceRow&) = default;
    };

    /// A built plan, ready to run. Owns the operator tree, so it can be
    /// moved (returned from a function) but not copied: a copy would mean
    /// two owners of the same pages.
    ///
    /// Destroying it without close() still releases every page: operators
    /// hold pages only in members that clean up after themselves.
    class ExecutablePlan
    {
    public:
        /// Use buildExecutablePlan, which does every check first.
        /// `labels` holds one label per node, in pre-order.
        ExecutablePlan(ExecContext& ctx, std::unique_ptr<Operator> root, std::vector<std::string> labels);

        /// Starts a new measurement of the most frames held, then opens the
        /// root (blocking operators do their work here).
        void open();
        /// Fills `out` with the next result tuple and returns true, or
        /// returns false at the end.
        bool next(Tuple& out);
        /// Closes the whole tree. Then checks SPEC §M2.1 "after close no
        /// pinned pages and no temporary files": if any are left, an
        /// operator has a bug, and this throws std::logic_error naming the
        /// problem. Assumes nothing outside this plan holds a page of the
        /// same pool. A second call only repeats the check.
        void close();

        /// The columns of the result tuples.
        const Schema& schema() const;

        /// Counters of the whole plan, for `run --json` (SPEC §M2.5).
        /// comparisons, pageReads and pageWrites: the sum over every node.
        /// tuplesOut: the root's, the number of result tuples.
        /// Still readable after close().
        ExecCounters totals() const;

        /// One row per node in pre-order: the root, then its first child's
        /// subtree, then its second child's subtree. Row i is the node with
        /// pre-order index i, the same index divideFrames and the optimizer
        /// use to match nodes with their estimates. Still readable after
        /// close().
        std::vector<TraceRow> trace() const;

        /// The most frames held at once since open(): pinned pages plus
        /// frames reserved for tuple buffers (the pool's peak, chunk 03).
        /// SPEC §M2.6 measurement 5.
        std::size_t maxFramesHeld() const;

    private:
        ExecContext& ctx_;
        std::unique_ptr<Operator> root_;
        std::vector<std::string> labels_;
    };

    /// Builds `physicalPlan` against the database in `ctx`, with B = the
    /// pool's frame count. In this order:
    /// 1. divideFrames: a plan that needs more than B frames ->
    ///    db::Error(Schema) (chunk 03 message).
    /// 2. Builds the operators, children before parents, so each one checks
    ///    its names against its children's schemas: unknown table ->
    ///    Error(Name); unknown column -> Error(Name) or Error(Ambiguity);
    ///    INT against VARCHAR -> Error(Type); schema errors -> Error(Schema).
    /// Nothing is opened, so on an error no data page has been read and no
    /// temporary file made. `ctx` must outlive the returned plan.
    ExecutablePlan buildExecutablePlan(const PlanNode& physicalPlan, ExecContext& ctx);
} // namespace db::exec
