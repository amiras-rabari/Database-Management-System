#include "db/opt/rewrite.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"

namespace db::opt
{
    bool enabled(const RuleSet& rules, RuleId rule)
    {
        return rules[static_cast<std::size_t>(rule) - 1];
    }

    namespace
    {
        /// What both passes need at every node.
        struct Context
        {
            const RuleSet& rules;
            const TableSchemaLookup& tableSchema;

            bool on(RuleId rule) const
            {
                return enabled(rules, rule);
            }

            /// The output columns of a subtree (common's schema rules).
            Schema schemaOf(const PlanNode& node) const
            {
                return outputSchema(node, tableSchema);
            }
        };

        // Both passes walk the tree recursively, one call per tree level,
        // and plans may be up to kMaxNestingDepth (1000) levels deep. So
        // the recursive functions keep almost nothing on the stack: each
        // builds its result in place in `out` (no PlanNode temporaries),
        // and the per-node work that needs schemas or condition lists is
        // done in a helper that has returned before the recursion goes on.
        // A run of stacked selects is walked with a loop, not recursion.

        /// Copies `node`'s own data (op, condition, columns, ...) into `out`,
        /// without its children. Copying `node` itself would copy its whole
        /// subtree only to throw the old children away.
        void copyNodeData(const PlanNode& node, PlanNode& out)
        {
            out.op = node.op;
            out.table = node.table;
            out.condition = node.condition;
            out.columns = node.columns;
            out.alias = node.alias;
            out.mode = node.mode;
            out.sortKeys = node.sortKeys;
            out.aggregates = node.aggregates;
            out.limitCount = node.limitCount;
        }

        /// A reference to a schema column, as a plan writes it: `e.name`, or
        /// a bare `n` for an aggregate output (which has no qualifier).
        ColumnRef refTo(const Column& column)
        {
            if (column.qualifier.empty())
            {
                return ColumnRef{std::nullopt, column.name};
            }
            return ColumnRef{column.qualifier, column.name};
        }

        /// True when `ref` names a column of `schema` (qualified: same
        /// qualifier and name; bare: same name). Like Schema::resolve, but
        /// answers instead of throwing.
        bool has(const Schema& schema, const ColumnRef& ref)
        {
            for (const Column& column : schema.columns())
            {
                if (column.name == ref.name && (!ref.qualifier || *ref.qualifier == column.qualifier))
                {
                    return true;
                }
            }
            return false;
        }

        /// `condition` with each column renamed to the column at the same
        /// position in `to`. A rename and a union keep positions: column i of
        /// `rename[t](X)` is column i of X, and column i of `union(L, R)` is
        /// column i of R on the right side.
        Condition mapByPosition(const Condition& condition, const Schema& from, const Schema& to)
        {
            return mapColumns(condition, [&from, &to](const ColumnRef& ref) {
                return refTo(to.column(from.resolve(ref)));
            });
        }

        /// Puts the conditions that stop here back as selects above `out`,
        /// the first one on top (their order in the original tree). With
        /// rule 7 on they become one select joined with `and`.
        void addSelectsAbove(PlanNode& out, std::vector<Condition> stays, const Context& ctx)
        {
            if (stays.empty())
            {
                return;
            }
            if (ctx.on(RuleId::RemoveRedundant))
            {
                PlanNode core = std::move(out);
                out = PlanNode::select(conjoin(std::move(stays)), std::move(core));
                return;
            }
            for (std::size_t i = stays.size(); i-- > 0;)
            {
                PlanNode core = std::move(out);
                out = PlanNode::select(std::move(stays[i]), std::move(core));
            }
        }

        // ---------------------------------------------------------------
        // Pass 1: selects (rules 1-5, and rule 7's select merge)
        // ---------------------------------------------------------------

        void pushSelects(const PlanNode& node, std::vector<Condition> pending, const Context& ctx, PlanNode& out);

        /// Which inputs of a join a condition reads.
        enum class Side
        {
            None,
            Left,
            Right,
            Both
        };

        Side sideOf(const Condition& condition, const Schema& left)
        {
            bool usesLeft = false;
            bool usesRight = false;
            for (const ColumnRef& ref : columnsUsed(condition))
            {
                // The plan is valid, so a column not on the left is on the right.
                if (has(left, ref))
                {
                    usesLeft = true;
                }
                else
                {
                    usesRight = true;
                }
            }
            if (usesLeft && usesRight)
            {
                return Side::Both;
            }
            if (usesLeft)
            {
                return Side::Left;
            }
            return usesRight ? Side::Right : Side::None;
        }

        /// Where the conditions met at a join or cross go.
        struct JoinSplit
        {
            std::vector<Condition> toLeft;
            std::vector<Condition> toRight;
            /// The conditions that end up in the join's own condition.
            std::vector<Condition> joinParts;
            std::vector<Condition> stays;
        };

        /// Rules 2 and 3 at a join or cross: sorts every condition in
        /// `pending` (and, with rule 3 on, the parts of a written join's
        /// condition) to the left input, the right input, the join's own
        /// condition, or "stays above".
        JoinSplit splitAtJoin(const PlanNode& node, std::vector<Condition> pending, const Context& ctx)
        {
            Schema leftSchema = ctx.schemaOf(node.children[0]);
            Schema rightSchema = ctx.schemaOf(node.children[1]);
            bool crossToJoin = ctx.on(RuleId::CrossToJoin);

            JoinSplit split;
            if (node.op == PlanOp::Join)
            {
                if (crossToJoin)
                {
                    // With rule 3 on, join[c](L, R) is read as
                    // select[c](cross(L, R)): c's parts are placed like any
                    // WHERE condition, below the ones from above. So
                    // `FROM e JOIN d ON ...` and `FROM e, d WHERE ...` give
                    // the same tree. The ON condition is always split here:
                    // left whole, `e.did = d.did and e.age > 60` is no
                    // equality, and the join would become a cross product.
                    for (Condition& part : splitConjuncts(*node.condition))
                    {
                        pending.push_back(std::move(part));
                    }
                }
                else
                {
                    // Rule 3 off: a written join keeps its condition as it is.
                    split.joinParts.push_back(*node.condition);
                }
            }

            for (Condition& condition : pending)
            {
                Side side = sideOf(condition, leftSchema);
                if (side == Side::Left && ctx.on(RuleId::PushSelectBelowJoin))
                {
                    // Rule 2: only left columns, so filtering the left input
                    // first keeps exactly the same pairs.
                    split.toLeft.push_back(std::move(condition));
                }
                else if (side == Side::Right && ctx.on(RuleId::PushSelectBelowJoin))
                {
                    split.toRight.push_back(std::move(condition));
                }
                else if (side == Side::Both && crossToJoin && equiJoinKeys(condition, leftSchema, rightSchema))
                {
                    // Rule 3: an equality between a left and a right column
                    // becomes (part of) the join condition.
                    split.joinParts.push_back(std::move(condition));
                }
                else
                {
                    // A constant, a both-sides non-equality, or a rule that
                    // is off: it stays above.
                    split.stays.push_back(std::move(condition));
                }
            }
            return split;
        }

        /// Rules 2 and 3 at a join or cross, building the result in `out`.
        void pushIntoJoin(const PlanNode& node, std::vector<Condition> pending, const Context& ctx, PlanNode& out)
        {
            JoinSplit split = splitAtJoin(node, std::move(pending), ctx);
            out.children.resize(2);
            pushSelects(node.children[0], std::move(split.toLeft), ctx, out.children[0]);
            pushSelects(node.children[1], std::move(split.toRight), ctx, out.children[1]);
            if (split.joinParts.empty())
            {
                out.op = PlanOp::Cross;
            }
            else
            {
                out.op = PlanOp::Join;
                out.condition = conjoin(std::move(split.joinParts));
            }
            addSelectsAbove(out, std::move(split.stays), ctx);
        }

        /// Rule 5 at a group_by: which conditions may move below it. A
        /// condition moves only if it uses at least one column and every
        /// column it uses is a grouping key (returned first; the rest,
        /// which stay above, in `stays`).
        /// - A condition on an aggregate (`n > 5`, from HAVING) never moves:
        ///   n does not exist before grouping.
        /// - A constant (`1 = 2`) never moves: with no keys, an empty input
        ///   still gives one row (count = 0), which `1 = 2` above removes
        ///   but `1 = 2` below would not.
        std::vector<Condition> splitAtGroupBy(const PlanNode& node, std::vector<Condition> pending,
                                              const Context& ctx, std::vector<Condition>& stays)
        {
            // The output is the keys (named as in the child), then the
            // aggregates; so output position < number of keys means "a key".
            Schema schema = ctx.schemaOf(node);
            std::size_t keyCount = node.columns.size();

            std::vector<Condition> down;
            for (Condition& condition : pending)
            {
                std::vector<ColumnRef> used = columnsUsed(condition);
                bool onlyKeys = !used.empty();
                for (const ColumnRef& ref : used)
                {
                    if (schema.resolve(ref) >= keyCount)
                    {
                        onlyKeys = false;
                    }
                }
                if (onlyKeys)
                {
                    down.push_back(std::move(condition));
                }
                else
                {
                    stays.push_back(std::move(condition));
                }
            }
            return down;
        }

        /// Rule 5 at a group_by, building the result in `out`.
        void pushIntoGroupBy(const PlanNode& node, std::vector<Condition> pending, const Context& ctx, PlanNode& out)
        {
            std::vector<Condition> stays;
            std::vector<Condition> down = splitAtGroupBy(node, std::move(pending), ctx, stays);
            copyNodeData(node, out);
            out.children.resize(1);
            pushSelects(node.children[0], std::move(down), ctx, out.children[0]);
            addSelectsAbove(out, std::move(stays), ctx);
        }

        /// `t.a > 5` above rename[t](X) becomes X's own name for that column
        /// below it (rule 4).
        std::vector<Condition> mapThroughRename(const PlanNode& node, std::vector<Condition> pending,
                                                const Context& ctx)
        {
            Schema renamed = ctx.schemaOf(node);
            Schema inner = ctx.schemaOf(node.children[0]);
            for (Condition& condition : pending)
            {
                condition = mapByPosition(condition, renamed, inner);
            }
            return pending;
        }

        /// The union's conditions rewritten for its right branch (rule 4):
        /// the union's column names are the left side's, so the right
        /// branch gets the same condition on its own columns, by position.
        std::vector<Condition> mapToRightBranch(const PlanNode& node, const std::vector<Condition>& pending,
                                                const Context& ctx)
        {
            Schema leftSchema = ctx.schemaOf(node.children[0]);
            Schema rightSchema = ctx.schemaOf(node.children[1]);
            std::vector<Condition> toRight;
            for (const Condition& condition : pending)
            {
                toRight.push_back(mapByPosition(condition, leftSchema, rightSchema));
            }
            return toRight;
        }

        /// Adds the condition of the select `node` to `pending`, after the
        /// ones carried from above, since it was lower in the tree. Rule 1
        /// splits it into its AND parts so each can move on its own.
        void takeCondition(const PlanNode& node, std::vector<Condition>& pending, const Context& ctx)
        {
            if (ctx.on(RuleId::SplitConjunctions))
            {
                for (Condition& part : splitConjuncts(*node.condition))
                {
                    pending.push_back(std::move(part));
                }
            }
            else
            {
                pending.push_back(*node.condition);
            }
        }

        /// Nothing from above may pass `node`: those conditions stay above
        /// it, and its children start with none.
        void stopHere(const PlanNode& node, std::vector<Condition> pending, const Context& ctx, PlanNode& out)
        {
            copyNodeData(node, out);
            out.children.resize(node.children.size());
            for (std::size_t i = 0; i < node.children.size(); ++i)
            {
                pushSelects(node.children[i], {}, ctx, out.children[i]);
            }
            addSelectsAbove(out, std::move(pending), ctx);
        }

        /// Places the conditions in `pending` (carried down from above, in
        /// top-to-bottom order) at or below `node`, and builds the new
        /// subtree in `out` (an empty node).
        void pushSelects(const PlanNode& node, std::vector<Condition> pending, const Context& ctx, PlanNode& out)
        {
            // Every select itself goes away: its condition joins the carried
            // ones. A run of stacked selects is taken in one loop.
            const PlanNode* below = &node;
            while (below->op == PlanOp::Select)
            {
                takeCondition(*below, pending, ctx);
                below = &below->children[0];
            }

            switch (below->op)
            {
            case PlanOp::Join:
            case PlanOp::Cross:
                pushIntoJoin(*below, std::move(pending), ctx, out);
                return;

            case PlanOp::Rename:
                // Rule 4, skipped over a bare scan: the §M4.3 example keeps
                // `select e.age>60` above `rename e (scan Emp)`, and a filter
                // costs nothing on either side of a rename.
                if (ctx.on(RuleId::PushSelectBelowRenameUnion) && below->children[0].op != PlanOp::Scan)
                {
                    std::vector<Condition> mapped = mapThroughRename(*below, std::move(pending), ctx);
                    copyNodeData(*below, out);
                    out.children.resize(1);
                    pushSelects(below->children[0], std::move(mapped), ctx, out.children[0]);
                    return;
                }
                break;

            case PlanOp::Union:
            case PlanOp::UnionAll:
                if (ctx.on(RuleId::PushSelectBelowRenameUnion))
                {
                    // Rule 4: a union's rows come from both sides, so the
                    // filter goes into BOTH branches.
                    std::vector<Condition> toRight = mapToRightBranch(*below, pending, ctx);
                    copyNodeData(*below, out);
                    out.children.resize(2);
                    pushSelects(below->children[0], std::move(pending), ctx, out.children[0]);
                    pushSelects(below->children[1], std::move(toRight), ctx, out.children[1]);
                    return;
                }
                break;

            case PlanOp::GroupBy:
                if (ctx.on(RuleId::PushSelectBelowGroupBy))
                {
                    pushIntoGroupBy(*below, std::move(pending), ctx, out);
                    return;
                }
                break;

            default:
                // project, distinct, sort, limit, intersect, except, scan:
                // nothing passes them. Below a limit it would change which
                // rows survive; the others are not in §M4.3's list.
                break;
            }
            stopHere(*below, std::move(pending), ctx, out);
        }

        // ---------------------------------------------------------------
        // Pass 2: projections (rule 6, and rule 7's projection removal)
        // ---------------------------------------------------------------

        /// Marks the columns `refs` name in `schema` as needed.
        void markUsed(std::vector<bool>& needed, const Schema& schema, const std::vector<ColumnRef>& refs)
        {
            for (const ColumnRef& ref : refs)
            {
                needed[schema.resolve(ref)] = true;
            }
        }

        void pushProjections(const PlanNode& node, const std::vector<bool>& needed, const Context& ctx,
                             PlanNode& out);

        /// Rule 6 at one join input: below it, a projection that keeps only
        /// the columns marked in `needed`, in the input's own column order.
        /// Nothing is inserted when it would drop nothing, or keep nothing
        /// (a projection of zero columns is not a valid plan).
        void narrowInput(const PlanNode& input, const std::vector<bool>& needed, const Context& ctx, PlanNode& out)
        {
            pushProjections(input, needed, ctx, out);
            if (!ctx.on(RuleId::PushProjections))
            {
                return;
            }

            Schema schema = ctx.schemaOf(input);
            std::vector<ColumnRef> kept;
            for (std::size_t i = 0; i < schema.size(); ++i)
            {
                if (needed[i])
                {
                    kept.push_back(refTo(schema.column(i)));
                }
            }
            if (kept.empty() || kept.size() == schema.size())
            {
                return;
            }
            PlanNode core = std::move(out);
            out = PlanNode::project(std::move(kept), std::move(core));
        }

        /// At a project: which columns of its child it reads, and (in
        /// `dropsNothing`) whether it keeps every column of its child in the
        /// same order.
        std::vector<bool> projectChildNeeded(const PlanNode& node, const Context& ctx, bool& dropsNothing)
        {
            Schema childSchema = ctx.schemaOf(node.children[0]);
            std::vector<std::size_t> picked;
            projectSchema(childSchema, node.columns, &picked);

            dropsNothing = picked.size() == childSchema.size();
            for (std::size_t i = 0; dropsNothing && i < picked.size(); ++i)
            {
                dropsNothing = picked[i] == i;
            }

            // The projection itself is kept whole, so every column it lists
            // must come from below.
            std::vector<bool> childNeeded(childSchema.size(), false);
            for (std::size_t i : picked)
            {
                childNeeded[i] = true;
            }
            return childNeeded;
        }

        /// The columns each input of a join or cross must produce.
        struct JoinNeeds
        {
            std::vector<bool> left;
            std::vector<bool> right;
        };

        JoinNeeds joinChildNeeded(const PlanNode& node, const std::vector<bool>& needed, const Context& ctx)
        {
            Schema leftSchema = ctx.schemaOf(node.children[0]);
            Schema rightSchema = ctx.schemaOf(node.children[1]);

            // The join's output is left columns then right columns, so the
            // first leftSchema.size() marks belong to the left side. Its
            // condition's columns are needed too (§M4.3 rule 6: never drop a
            // column a predicate still references).
            std::vector<bool> all = needed;
            if (node.op == PlanOp::Join)
            {
                markUsed(all, concatSchemas(leftSchema, rightSchema), columnsUsed(*node.condition));
            }
            JoinNeeds needs;
            needs.left.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(leftSchema.size()));
            needs.right.assign(all.begin() + static_cast<std::ptrdiff_t>(leftSchema.size()), all.end());
            return needs;
        }

        /// At a one-input or set-operation node: which columns of `child`
        /// must be produced, given the node's own `needed` columns.
        std::vector<bool> childNeededAt(const PlanNode& node, const PlanNode& child, const std::vector<bool>& needed,
                                        const Context& ctx)
        {
            Schema childSchema = ctx.schemaOf(child);
            std::vector<bool> childNeeded(childSchema.size(), false);

            switch (node.op)
            {
            case PlanOp::Select:
                // Same columns as the child, plus the ones it tests.
                childNeeded = needed;
                markUsed(childNeeded, childSchema, columnsUsed(*node.condition));
                break;
            case PlanOp::Rename:
            case PlanOp::Limit:
                // Same columns in the same positions.
                childNeeded = needed;
                break;
            case PlanOp::Sort:
                childNeeded = needed;
                for (const SortKey& key : node.sortKeys)
                {
                    childNeeded[childSchema.resolve(key.column)] = true;
                }
                break;
            case PlanOp::GroupBy:
                // The keys and every aggregate's input (count(*) reads none).
                markUsed(childNeeded, childSchema, node.columns);
                for (const Aggregate& aggregate : node.aggregates)
                {
                    if (aggregate.column)
                    {
                        childNeeded[childSchema.resolve(*aggregate.column)] = true;
                    }
                }
                break;
            default:
                // distinct, union, union_all, intersect, except: whether two
                // rows are duplicates depends on every column, and a union's
                // branches must stay aligned, so all are needed.
                childNeeded.assign(childSchema.size(), true);
                break;
            }
            return childNeeded;
        }

        /// Rewrites the subtree at `node` into `out` (an empty node), where
        /// needed[i] says whether output column i of `node` is still used by
        /// something above. Each node works out which columns its children
        /// must produce: the ones above needs, plus the ones it reads itself.
        void pushProjections(const PlanNode& node, const std::vector<bool>& needed, const Context& ctx,
                             PlanNode& out)
        {
            switch (node.op)
            {
            case PlanOp::Scan:
                out = node;
                return;

            case PlanOp::Project: {
                bool dropsNothing = false;
                std::vector<bool> childNeeded = projectChildNeeded(node, ctx, dropsNothing);
                // Rule 7: a projection keeping every column in the same
                // order drops nothing. One that reorders changes what the
                // user sees, so it stays.
                if (dropsNothing && ctx.on(RuleId::RemoveRedundant))
                {
                    // Same columns, same positions: the child is needed exactly as this node was.
                    pushProjections(node.children[0], needed, ctx, out);
                    return;
                }
                copyNodeData(node, out);
                out.children.resize(1);
                pushProjections(node.children[0], childNeeded, ctx, out.children[0]);
                return;
            }

            case PlanOp::Join:
            case PlanOp::Cross: {
                JoinNeeds needs = joinChildNeeded(node, needed, ctx);
                copyNodeData(node, out);
                out.children.resize(2);
                narrowInput(node.children[0], needs.left, ctx, out.children[0]);
                narrowInput(node.children[1], needs.right, ctx, out.children[1]);
                return;
            }

            default:
                break;
            }

            // One-input and set-operation nodes: work out what each child
            // must produce, then recurse.
            copyNodeData(node, out);
            out.children.resize(node.children.size());
            for (std::size_t i = 0; i < node.children.size(); ++i)
            {
                std::vector<bool> childNeeded = childNeededAt(node, node.children[i], needed, ctx);
                pushProjections(node.children[i], childNeeded, ctx, out.children[i]);
            }
        }
    } // namespace

    PlanNode rewritePlan(const PlanNode& plan, const RuleSet& rules, const TableSchemaLookup& tableSchema)
    {
        Context ctx{rules, tableSchema};

        PlanNode result;
        pushSelects(plan, {}, ctx, result);

        if (ctx.on(RuleId::PushProjections) || ctx.on(RuleId::RemoveRedundant))
        {
            // The user sees every column of the root, so all are needed.
            std::vector<bool> all(ctx.schemaOf(result).size(), true);
            PlanNode narrowed;
            pushProjections(result, all, ctx, narrowed);
            result = std::move(narrowed);
        }
        return result;
    }
} // namespace db::opt
