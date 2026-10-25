#pragma once

/// @file
/// The logical plan tree: the ten Phase 0 node types, their one-line text
/// form, and the output schema of a plan.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db/common/condition.h"
#include "db/common/schema.h"

namespace db
{

    /// What a plan node does.
    enum class PlanOp
    {
        /// Reads a stored table.
        Scan,
        /// Keeps the tuples of its child that satisfy a condition.
        Select,
        /// Keeps the listed columns of its child (duplicates stay).
        Project,
        /// Pairs of left and right tuples that satisfy a condition.
        Join,
        /// Every pair of a left and a right tuple.
        Cross,
        /// Its child with every column qualified by a new name.
        Rename,
        /// Tuples in the left or the right child.
        Union,
        /// Tuples in both children.
        Intersect,
        /// Tuples of the left child that are not in the right one.
        Except,
        /// Its child with duplicate tuples removed.
        Distinct
    };

    /// The logical name of an operation, as later modules (M3) use it:
    /// `scan select project join cross rename union intersect except distinct`.
    std::string_view opName(PlanOp op);

    /// A plan tree node. Copyable, no pointers. Build nodes with the named
    /// constructors; the fields an op does not use keep their defaults.
    struct PlanNode
    {
        /// What this node does.
        PlanOp op = PlanOp::Scan;
        /// The table a Scan reads.
        std::string table;
        /// The condition of a Select or Join.
        std::optional<Condition> condition;
        /// The columns of a Project, as written (qualified or bare).
        std::vector<ColumnRef> columns;
        /// The new name of a Rename.
        std::string alias;
        /// Inputs: none for Scan; one for Select, Project, Rename and
        /// Distinct; left then right for Join, Cross, Union, Intersect and
        /// Except.
        std::vector<PlanNode> children;

        /// Reads the stored table `table`.
        static PlanNode scan(std::string table);
        /// The tuples of `child` for which `condition` holds.
        static PlanNode select(Condition condition, PlanNode child);
        /// The `columns` of `child`, in that order.
        static PlanNode project(std::vector<ColumnRef> columns, PlanNode child);
        /// The pairs of `left` and `right` tuples for which `condition` holds.
        static PlanNode join(Condition condition, PlanNode left, PlanNode right);
        /// Every pair of a `left` and a `right` tuple.
        static PlanNode cross(PlanNode left, PlanNode right);
        /// `child` with every column qualified by `alias`.
        static PlanNode rename(std::string alias, PlanNode child);
        /// `left union right` (`union` itself is a C++ keyword).
        static PlanNode unionOf(PlanNode left, PlanNode right);
        /// `left intersect right`.
        static PlanNode intersect(PlanNode left, PlanNode right);
        /// `left minus right` in Bonus 1 syntax.
        static PlanNode except(PlanNode left, PlanNode right);
        /// `child` without duplicate tuples.
        static PlanNode distinct(PlanNode child);
    };

    /// True when both trees have the same shape, ops and node data (fields a
    /// node's op does not use are ignored). Children are compared in order,
    /// so `union(A, B)` and `union(B, A)` differ.
    bool operator==(const PlanNode& left, const PlanNode& right);

    /// The plan on one line: `scan(R)`, `select[COND](CHILD)`,
    /// `project[a, b](CHILD)`, `rename[E2](CHILD)`, `join[COND](L, R)`,
    /// `cross(L, R)`, `union(L, R)`, `intersect(L, R)`, `except(L, R)`,
    /// `distinct(CHILD)`. Conditions print in `style`; column refs print as
    /// written. Throws std::logic_error for a node without its condition.
    std::string toString(const PlanNode& plan, ConditionStyle style = ConditionStyle::Plan);

    /// The schema of the plan's result, computed bottom-up (children before
    /// the node, left before right, so the first error found is always the
    /// same):
    /// - Scan: `tableSchema(table)`; std::nullopt is Error(Name)
    ///   "Unknown relation 'X'.".
    /// - Select: the child's schema; the condition must bind to it
    ///   (bindCondition: Name, Ambiguity and Type errors).
    /// - Project: projectSchema (resolution errors, Error(Schema) for a column
    ///   named twice).
    /// - Join: concatSchemas (Error(Schema) for a qualified name on both
    ///   sides); the condition must bind with bindJoinCondition.
    /// - Cross: concatSchemas.
    /// - Rename: requalify with the alias.
    /// - Union, Intersect, Except: the left schema, after
    ///   checkUnionCompatible (Error(Schema)).
    /// - Distinct: the child's schema.
    /// Throws std::logic_error for a malformed node (wrong number of
    /// children, missing condition).
    Schema outputSchema(const PlanNode& plan,
                        const std::function<std::optional<Schema>(const std::string&)>& tableSchema);

} // namespace db
