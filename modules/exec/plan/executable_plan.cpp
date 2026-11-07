#include "db/exec/executable_plan.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "db/common/plan.h"
#include "db/exec/frame_plan.h"
#include "operators/hash_distinct.h"
#include "operators/pipelined.h"
#include "sort/set_ops.h"
#include "sort/sort_operator.h"

namespace db::exec
{
    namespace
    {
        // "seqscan R" for a seqscan (SPEC §M2.5 trace example), otherwise
        // the op name as plan JSON spells it.
        std::string labelOf(const PlanNode& node)
        {
            std::string label(opName(node.op));
            if (node.op == PlanOp::SeqScan)
                label += " " + node.table;
            return label;
        }

        // Builds the operator for `node` and everything below it.
        //
        // The label is added first, before the children are built, so the
        // labels end up in pre-order (node, left subtree, right subtree):
        // the same order trace() walks the operators in.
        //
        // The children are built before this node's operator, because each
        // constructor checks its names against its children's schemas.
        //
        // `frames` holds every node's FrameShare in the same pre-order, so
        // labels.size() before the push is this node's index in it.
        //
        // Recursive, one call per plan node (up to 1000 deep), so the frame
        // stays small: the node is passed by reference and nothing is copied.
        std::unique_ptr<Operator> buildNode(const PlanNode& node, ExecContext& ctx, const FramePlan& frames,
                                            std::vector<std::string>& labels)
        {
            const FrameShare& share = frames.shares[labels.size()];
            labels.push_back(labelOf(node));

            // The physical JSON reader guarantees the fields each op needs
            // (a filter has a condition, a union_all has two children, ...).
            // No default case: the compiler then warns if a PlanOp is missing.
            switch (node.op)
            {
            case PlanOp::SeqScan:
                return std::make_unique<SeqScanOperator>(ctx, node.table);
            case PlanOp::Filter:
                return std::make_unique<FilterOperator>(ctx, buildNode(node.children[0], ctx, frames, labels), *node.condition);
            case PlanOp::Project:
                return std::make_unique<ProjectOperator>(ctx, buildNode(node.children[0], ctx, frames, labels), node.columns);
            case PlanOp::Limit:
                return std::make_unique<LimitOperator>(ctx, buildNode(node.children[0], ctx, frames, labels), node.limitCount);
            case PlanOp::Rename:
                return std::make_unique<RenameOperator>(ctx, buildNode(node.children[0], ctx, frames, labels), node.alias);
            case PlanOp::UnionAll:
            {
                // Two separate statements: C++ may evaluate function
                // arguments in any order, and the left child must be built
                // (and labelled) first.
                std::unique_ptr<Operator> left = buildNode(node.children[0], ctx, frames, labels);
                std::unique_ptr<Operator> right = buildNode(node.children[1], ctx, frames, labels);
                return std::make_unique<UnionAllOperator>(ctx, std::move(left), std::move(right));
            }
            case PlanOp::Sort:
                return std::make_unique<SortOperator>(ctx, buildNode(node.children[0], ctx, frames, labels),
                                                      node.sortKeys, share);
            case PlanOp::Distinct:
                // The plan reader requires `mode` (DECISIONS 2026-11-01).
                if (*node.mode == ExecMode::Hash)
                    return std::make_unique<HashDistinctOperator>(
                        ctx, buildNode(node.children[0], ctx, frames, labels), share);
                return std::make_unique<DedupSortOperator>(
                    ctx, oneChild(buildNode(node.children[0], ctx, frames, labels)), share);
            case PlanOp::Union:
            case PlanOp::Intersect:
            case PlanOp::Except:
            {
                // Left built first, as for union_all.
                std::unique_ptr<Operator> left = buildNode(node.children[0], ctx, frames, labels);
                std::unique_ptr<Operator> right = buildNode(node.children[1], ctx, frames, labels);
                if (node.op == PlanOp::Union)
                    return std::make_unique<DedupSortOperator>(ctx, twoChildren(std::move(left), std::move(right)),
                                                               share);
                const SetMergeOperator::Kind kind = node.op == PlanOp::Intersect ? SetMergeOperator::Kind::Intersect
                                                                                 : SetMergeOperator::Kind::Except;
                return std::make_unique<SetMergeOperator>(ctx, std::move(left), std::move(right), kind, share);
            }

            // Written in chunks 09-13; each one replaces its line here.
            case PlanOp::GroupBy:
            case PlanOp::NestedLoopJoin:
            case PlanOp::SortMergeJoin:
            case PlanOp::HashJoin:
            // Logical ops never get here: divideFrames already rejected them.
            case PlanOp::Scan:
            case PlanOp::Select:
            case PlanOp::Join:
            case PlanOp::Cross:
                break;
            }
            throw std::logic_error("Operator '" + std::string(opName(node.op)) + "' is not built yet.");
        }

        // Adds one trace row for `op`, then for each child's subtree, left
        // first: pre-order. rows.size() is this node's pre-order index, so
        // labels[rows.size()] is the label buildNode made for this node.
        void addTraceRows(const Operator& op, std::size_t depth, const std::vector<std::string>& labels,
                          std::vector<TraceRow>& rows)
        {
            rows.push_back({depth, labels[rows.size()], op.counters(), subtreeCounters(op)});
            for (const std::unique_ptr<Operator>& child : op.children())
                addTraceRows(*child, depth + 1, labels, rows);
        }
    } // namespace

    ExecutablePlan::ExecutablePlan(ExecContext& ctx, std::unique_ptr<Operator> root, std::vector<std::string> labels)
        : ctx_(ctx), root_(std::move(root)), labels_(std::move(labels))
    {
    }

    void ExecutablePlan::open()
    {
        // The peak now starts from what is held before the plan runs.
        ctx_.pool().resetPeak();
        root_->open();
    }

    bool ExecutablePlan::next(Tuple& out)
    {
        return root_->next(out);
    }

    void ExecutablePlan::close()
    {
        root_->close();
        // Every operator releases its pages and files in its own close, so
        // these only fire if an operator has a bug (user approved keeping
        // them, chunk 05 Q2 (a)).
        if (ctx_.pool().pinnedFrameCount() != 0)
            throw std::logic_error("Plan closed with pages still pinned.");
        if (ctx_.engine().liveTempFileCount() != 0)
            throw std::logic_error("Plan closed with temporary files left.");
    }

    const Schema& ExecutablePlan::schema() const
    {
        return root_->schema();
    }

    ExecCounters ExecutablePlan::totals() const
    {
        // subtreeCounters sums every node; tuples_out is replaced by the
        // root's own count, the number of result rows (SPEC §M2.5 run --json).
        ExecCounters totals = subtreeCounters(*root_);
        totals.tuplesOut = root_->counters().tuplesOut;
        return totals;
    }

    std::vector<TraceRow> ExecutablePlan::trace() const
    {
        std::vector<TraceRow> rows;
        addTraceRows(*root_, 0, labels_, rows);
        return rows;
    }

    std::size_t ExecutablePlan::maxFramesHeld() const
    {
        return ctx_.pool().peakPinnedCount();
    }

    ExecutablePlan buildExecutablePlan(const PlanNode& physicalPlan, ExecContext& ctx)
    {
        // 1. Frames first: a plan that cannot fit in B is rejected before
        //    any table is looked up.
        //    The shares are passed to the operators that need a number of
        //    frames (sort, from chunk 07).
        const FramePlan frames = divideFrames(physicalPlan, ctx.pool().frameCount());

        // 2. Build the operators. Any name, type or schema error is thrown
        //    here, before anything is opened.
        std::vector<std::string> labels;
        std::unique_ptr<Operator> root = buildNode(physicalPlan, ctx, frames, labels);
        return ExecutablePlan(ctx, std::move(root), std::move(labels));
    }
} // namespace db::exec
