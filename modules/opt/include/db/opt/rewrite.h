#pragma once

/// @file
/// The seven rewrite rules of §M4.3, applied to a logical plan before any
/// costing. Each rule can be switched off on its own (§M4.7 #3 measures how
/// much each one matters). "A rule applied where its precondition fails
/// produces wrong answers, not slow ones", so every rule checks its
/// precondition first.
///
/// | # | Rule | Precondition |
/// |---|---|---|
/// | 1 | split `select[p and q]` into stacked selects | none |
/// | 2 | push a select below a join or cross | it uses columns of one side only |
/// | 3 | cross + select becomes a join | it uses both sides and is an equality |
/// | 4 | push a select below a rename, union or union_all | names rewritten through the rename; into both union branches |
/// | 5 | push a select below a group_by | it uses grouping columns alone |
/// | 6 | push projections down | never drop a column something above still uses |
/// | 7 | remove a projection that drops nothing; merge adjacent selects | cosmetic |

#include <array>
#include <functional>
#include <optional>
#include <string>

#include "db/common/plan.h"
#include "db/common/schema.h"

namespace db::opt
{
    /// The rules, numbered as in §M4.3.
    enum class RuleId
    {
        SplitConjunctions = 1,
        PushSelectBelowJoin = 2,
        CrossToJoin = 3,
        PushSelectBelowRenameUnion = 4,
        PushSelectBelowGroupBy = 5,
        PushProjections = 6,
        RemoveRedundant = 7
    };

    /// Which rules are on: entry (rule number - 1).
    using RuleSet = std::array<bool, 7>;

    /// Every rule on: the normal optimizer.
    inline constexpr RuleSet kAllRules = {true, true, true, true, true, true, true};

    /// True when `rule` is on in `rules`.
    bool enabled(const RuleSet& rules, RuleId rule);

    /// A table's schema by name, or std::nullopt (common's outputSchema
    /// takes the same kind of function).
    using TableSchemaLookup = std::function<std::optional<Schema>(const std::string&)>;

    /// Returns the rewritten plan; `plan` itself is not changed, so explain
    /// can show both trees (§M4.3). `plan` must be a valid logical plan
    /// (outputSchema accepts it); the result has the same output columns in
    /// the same order and gives the same rows.
    ///
    /// Two passes, always in this order:
    /// 1. Select pass (rules 1-5, and 7's select merge). Walks the tree from
    ///    the top, carrying the select conditions met so far downward and
    ///    dropping each one as deep as its precondition allows:
    ///    - below a join or cross, to the one side whose columns it uses
    ///      (rule 2); into the join condition if it is an equality across a
    ///      cross (rule 3). With rule 3 on, a written `join[c]` is first
    ///      treated as `select[c](cross)`, so a comma join and JOIN ON give
    ///      the same tree; with rule 3 off a join stays a join;
    ///    - below a rename (names rewritten by position) and into both
    ///      branches of a union / union_all (rule 4). Not below a rename
    ///      whose child is a bare scan: §M4.3's example keeps
    ///      `select e.age>60` above `rename e (scan Emp)`;
    ///    - below a group_by, when every column it uses is a grouping key,
    ///      and the group_by has keys (rule 5);
    ///    - never below project, distinct, sort, limit, intersect or except.
    ///    Conditions that stop at a node become stacked selects above it, in
    ///    their top-to-bottom order, or one select joined with `and` when
    ///    rule 7 is on.
    /// 2. Projection pass (rule 6, and 7's projection removal). Walks the
    ///    tree from the top with the set of columns each node must still
    ///    produce. Under each join and cross input it inserts
    ///    `project[needed columns]` (in that input's column order) when that
    ///    drops at least one column (rule 6). A project that keeps every
    ///    column of its input in the same order is removed (rule 7).
    PlanNode rewritePlan(const PlanNode& plan, const RuleSet& rules, const TableSchemaLookup& tableSchema);
} // namespace db::opt
