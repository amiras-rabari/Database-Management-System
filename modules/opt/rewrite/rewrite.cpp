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

        /// A copy of `node`'s own data (op, condition, columns, ...) with
        /// new children. Copying `node` itself would copy its whole subtree
        /// only to throw the old children away.
        PlanNode withChildren(const PlanNode& node, std::vector<PlanNode> children)
        {
            PlanNode out;
            out.op = node.op;
            out.table = node.table;
            out.condition = node.condition;
            out.columns = node.columns;
            out.alias = node.alias;
            out.mode = node.mode;
            out.sortKeys = node.sortKeys;
            out.aggregates = node.aggregates;
            out.limitCount = node.limitCount;
            out.children = std::move(children);
            return out;
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

        /// Puts the conditions that stop here back as selects above `core`,
        /// the first one on top (their order in the original tree). With
        /// rule 7 on they become one select joined with `and`.
        PlanNode withSelects(PlanNode core, std::vector<Condition> stays, const Context& ctx)
        {
            if (stays.empty())
            {
                return core;
            }
            if (ctx.on(RuleId::RemoveRedundant))
            {
                return PlanNode::select(conjoin(std::move(stays)), std::move(core));
            }
            for (std::size_t i = stays.size(); i-- > 0;)
            {
                core = PlanNode::select(std::move(stays[i]), std::move(core));
            }
            return core;
        }

        // ---------------------------------------------------------------
        // Pass 1: selects (rules 1-5, and rule 7's select merge)
        // ---------------------------------------------------------------

        PlanNode pushSelects(const PlanNode& node, std::vector<Condition> pending, const Context& ctx);

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

        /// Rules 2 and 3 at a join or cross.
        PlanNode pushIntoJoin(const PlanNode& node, std::vector<Condition> pending, const Context& ctx)
        {
            const PlanNode& left = node.children[0];
            const PlanNode& right = node.children[1];
            Schema leftSchema = ctx.schemaOf(left);
            Schema rightSchema = ctx.schemaOf(right);
            bool crossToJoin = ctx.on(RuleId::CrossToJoin);

            // The conditions that end up in the join's own condition.
            std::vector<Condition> joinParts;
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
                    joinParts.push_back(*node.condition);
                }
            }

            std::vector<Condition> toLeft;
            std::vector<Condition> toRight;
            std::vector<Condition> stays;
            for (Condition& condition : pending)
            {
                Side side = sideOf(condition, leftSchema);
                if (side == Side::Left && ctx.on(RuleId::PushSelectBelowJoin))
                {
                    // Rule 2: only left columns, so filtering the left input
                    // first keeps exactly the same pairs.
                    toLeft.push_back(std::move(condition));
                }
                else if (side == Side::Right && ctx.on(RuleId::PushSelectBelowJoin))
                {
                    toRight.push_back(std::move(condition));
                }
                else if (side == Side::Both && crossToJoin && equiJoinKeys(condition, leftSchema, rightSchema))
                {
                    // Rule 3: an equality between a left and a right column
                    // becomes (part of) the join condition.
                    joinParts.push_back(std::move(condition));
                }
                else
                {
                    // A constant, a both-sides non-equality, or a rule that
                    // is off: it stays above.
                    stays.push_back(std::move(condition));
                }
            }

            PlanNode newLeft = pushSelects(left, std::move(toLeft), ctx);
            PlanNode newRight = pushSelects(right, std::move(toRight), ctx);
            PlanNode core = joinParts.empty()
                                ? PlanNode::cross(std::move(newLeft), std::move(newRight))
                                : PlanNode::join(conjoin(std::move(joinParts)), std::move(newLeft), std::move(newRight));
            return withSelects(std::move(core), std::move(stays), ctx);
        }

        /// Rule 5 at a group_by: a condition moves below only if it uses at
        /// least one column and every column it uses is a grouping key.
        /// - A condition on an aggregate (`n > 5`, from HAVING) never moves:
        ///   n does not exist before grouping.
        /// - A constant (`1 = 2`) never moves: with no keys, an empty input
        ///   still gives one row (count = 0), which `1 = 2` above removes
        ///   but `1 = 2` below would not.
        PlanNode pushIntoGroupBy(const PlanNode& node, std::vector<Condition> pending, const Context& ctx)
        {
            // The output is the keys (named as in the child), then the
            // aggregates; so output position < number of keys means "a key".
            Schema schema = ctx.schemaOf(node);
            std::size_t keyCount = node.columns.size();

            std::vector<Condition> down;
            std::vector<Condition> stays;
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

            PlanNode child = pushSelects(node.children[0], std::move(down), ctx);
            std::vector<PlanNode> children;
            children.push_back(std::move(child));
            return withSelects(withChildren(node, std::move(children)), std::move(stays), ctx);
        }

        /// Places the conditions in `pending` (carried down from above, in
        /// top-to-bottom order) at or below `node`, and returns the new
        /// subtree.
        PlanNode pushSelects(const PlanNode& node, std::vector<Condition> pending, const Context& ctx)
        {
            switch (node.op)
            {
            case PlanOp::Select:
                // The select itself goes away: its condition joins the
                // carried ones, after them, since it was lower in the tree.
                // Rule 1 splits it into its AND parts so each can move on
                // its own.
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
                return pushSelects(node.children[0], std::move(pending), ctx);

            case PlanOp::Join:
            case PlanOp::Cross:
                return pushIntoJoin(node, std::move(pending), ctx);

            case PlanOp::Rename:
                // Rule 4, skipped over a bare scan: the §M4.3 example keeps
                // `select e.age>60` above `rename e (scan Emp)`, and a filter
                // costs nothing on either side of a rename.
                if (ctx.on(RuleId::PushSelectBelowRenameUnion) && node.children[0].op != PlanOp::Scan)
                {
                    // `t.a > 5` above rename[t](X) becomes X's own name for
                    // that column below it.
                    Schema renamed = ctx.schemaOf(node);
                    Schema inner = ctx.schemaOf(node.children[0]);
                    for (Condition& condition : pending)
                    {
                        condition = mapByPosition(condition, renamed, inner);
                    }
                    std::vector<PlanNode> children;
                    children.push_back(pushSelects(node.children[0], std::move(pending), ctx));
                    return withChildren(node, std::move(children));
                }
                break;

            case PlanOp::Union:
            case PlanOp::UnionAll:
                if (ctx.on(RuleId::PushSelectBelowRenameUnion))
                {
                    // Rule 4: a union's rows come from both sides, so the
                    // filter goes into BOTH branches. The union's column
                    // names are the left side's; the right branch gets the
                    // same condition on its own columns, by position.
                    Schema leftSchema = ctx.schemaOf(node.children[0]);
                    Schema rightSchema = ctx.schemaOf(node.children[1]);
                    std::vector<Condition> toRight;
                    for (const Condition& condition : pending)
                    {
                        toRight.push_back(mapByPosition(condition, leftSchema, rightSchema));
                    }
                    std::vector<PlanNode> children;
                    children.push_back(pushSelects(node.children[0], std::move(pending), ctx));
                    children.push_back(pushSelects(node.children[1], std::move(toRight), ctx));
                    return withChildren(node, std::move(children));
                }
                break;

            case PlanOp::GroupBy:
                if (ctx.on(RuleId::PushSelectBelowGroupBy))
                {
                    return pushIntoGroupBy(node, std::move(pending), ctx);
                }
                break;

            default:
                // project, distinct, sort, limit, intersect, except, scan:
                // nothing passes them. Below a limit it would change which
                // rows survive; the others are not in §M4.3's list.
                break;
            }

            // Nothing from above may pass this node: those conditions stay
            // above it, and its children start with none.
            std::vector<PlanNode> children;
            for (const PlanNode& child : node.children)
            {
                children.push_back(pushSelects(child, {}, ctx));
            }
            return withSelects(withChildren(node, std::move(children)), std::move(pending), ctx);
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

        PlanNode pushProjections(const PlanNode& node, const std::vector<bool>& needed, const Context& ctx);

        /// Rule 6 at one join input: below it, a projection that keeps only
        /// the columns marked in `needed`, in the input's own column order.
        /// Nothing is inserted when it would drop nothing, or keep nothing
        /// (a projection of zero columns is not a valid plan).
        PlanNode narrowInput(const PlanNode& input, const std::vector<bool>& needed, const Context& ctx)
        {
            Schema schema = ctx.schemaOf(input);
            PlanNode result = pushProjections(input, needed, ctx);
            if (!ctx.on(RuleId::PushProjections))
            {
                return result;
            }

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
                return result;
            }
            return PlanNode::project(std::move(kept), std::move(result));
        }

        /// Rewrites the subtree at `node`, where needed[i] says whether
        /// output column i of `node` is still used by something above. Each
        /// node works out which columns its children must produce: the ones
        /// above needs, plus the ones it reads itself.
        PlanNode pushProjections(const PlanNode& node, const std::vector<bool>& needed, const Context& ctx)
        {
            switch (node.op)
            {
            case PlanOp::Scan:
                return node;

            case PlanOp::Project:
            {
                const PlanNode& child = node.children[0];
                Schema childSchema = ctx.schemaOf(child);
                std::vector<std::size_t> picked;
                projectSchema(childSchema, node.columns, &picked);

                // Rule 7: a projection keeping every column in the same
                // order drops nothing. One that reorders changes what the
                // user sees, so it stays.
                bool dropsNothing = picked.size() == childSchema.size();
                for (std::size_t i = 0; dropsNothing && i < picked.size(); ++i)
                {
                    dropsNothing = picked[i] == i;
                }
                if (dropsNothing && ctx.on(RuleId::RemoveRedundant))
                {
                    // Same columns, same positions: the child is needed exactly as this node was.
                    return pushProjections(child, needed, ctx);
                }

                // The projection itself is kept whole, so every column it
                // lists must come from below.
                std::vector<bool> childNeeded(childSchema.size(), false);
                for (std::size_t i : picked)
                {
                    childNeeded[i] = true;
                }
                std::vector<PlanNode> children;
                children.push_back(pushProjections(child, childNeeded, ctx));
                return withChildren(node, std::move(children));
            }

            case PlanOp::Join:
            case PlanOp::Cross:
            {
                Schema leftSchema = ctx.schemaOf(node.children[0]);
                Schema rightSchema = ctx.schemaOf(node.children[1]);

                // The join's output is left columns then right columns, so
                // the first leftSchema.size() marks belong to the left side.
                // Its condition's columns are needed too (§M4.3 rule 6:
                // never drop a column a predicate still references).
                std::vector<bool> all = needed;
                if (node.op == PlanOp::Join)
                {
                    markUsed(all, concatSchemas(leftSchema, rightSchema), columnsUsed(*node.condition));
                }
                std::vector<bool> leftNeeded(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(leftSchema.size()));
                std::vector<bool> rightNeeded(all.begin() + static_cast<std::ptrdiff_t>(leftSchema.size()), all.end());

                std::vector<PlanNode> children;
                children.push_back(narrowInput(node.children[0], leftNeeded, ctx));
                children.push_back(narrowInput(node.children[1], rightNeeded, ctx));
                return withChildren(node, std::move(children));
            }

            default:
                break;
            }

            // One-input and set-operation nodes: work out what each child
            // must produce, then recurse.
            std::vector<PlanNode> children;
            for (const PlanNode& child : node.children)
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
                    // distinct, union, union_all, intersect, except: whether
                    // two rows are duplicates depends on every column, and a
                    // union's branches must stay aligned, so all are needed.
                    childNeeded.assign(childSchema.size(), true);
                    break;
                }
                children.push_back(pushProjections(child, childNeeded, ctx));
            }
            return withChildren(node, std::move(children));
        }
    } // namespace

    PlanNode rewritePlan(const PlanNode& plan, const RuleSet& rules, const TableSchemaLookup& tableSchema)
    {
        Context ctx{rules, tableSchema};

        PlanNode result = pushSelects(plan, {}, ctx);

        if (ctx.on(RuleId::PushProjections) || ctx.on(RuleId::RemoveRedundant))
        {
            // The user sees every column of the root, so all are needed.
            std::vector<bool> all(ctx.schemaOf(result).size(), true);
            result = pushProjections(result, all, ctx);
        }
        return result;
    }
} // namespace db::opt
