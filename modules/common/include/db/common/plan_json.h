#pragma once

/// @file
/// Plan trees as JSON, the contract between every pair of modules. This is
/// the only place in the project where plan JSON field names are defined.
///
/// One object per node, `op` first, then the node's own data, then its
/// inputs (the order of the spec's examples):
///
/// | op | fields |
/// |---|---|
/// | `scan` | `table` |
/// | `select` | `condition`, `child` |
/// | `project` | `columns`, `child` |
/// | `join` | `condition`, `left`, `right` |
/// | `cross` | `left`, `right` |
/// | `rename` | `to`, `child` |
/// | `union`, `union_all`, `intersect`, `except` | `left`, `right` |
/// | `distinct` | `child` (physical: `mode`, `child`) |
/// | `group_by` | `keys` = `["Emp.did"]`, `aggregates` = `[["count","*","n"], ["avg","Emp.salary","avg_sal"]]` ([function, column, alias], sql example 5.4), `child` |
/// | `sort` | `keys` = `[["n","desc"], ["Emp.did","asc"]]`, `child` |
/// | `limit` | `n` (0 or more), `child` |
///
/// Physical plans (exec SPEC §M2.2–§M2.3) use `seqscan`, `filter`, the three
/// joins below, and `sort`, `limit`, `union_all` with the same fields as
/// above:
///
/// | op | fields |
/// |---|---|
/// | `seqscan` | `table` |
/// | `filter` | `condition`, `child` |
/// | `group_by` | `mode`, `keys` = `["R.dept"]`, `aggregates` = `[["count","*"], ["avg","R.salary"]]` ([function, column], no alias), `child` |
/// | `nested_loop_join`, `sort_merge_join`, `hash_join` | `condition`, `left`, `right` |
///
/// `mode` is `"sort"` or `"hash"` and is required in physical plans and
/// rejected in logical ones. The writer tells the two group_by forms apart
/// by `mode`: no mode means logical, so each aggregate gets its alias.
/// Physical plans also accept `project`, `union`, `intersect`, `except`
/// and, as our extension for self-joins, `rename` (DECISIONS.md 2026-11-01).
///
/// `condition` is a string in plan style (toString with ConditionStyle::Plan,
/// e.g. `"R.a > 500 and R.b = 'cs'"`); `columns` is an array of column
/// strings as written (`"Emp.name"` or `"name"`). Names with no JSON example
/// of their own follow the spec's pattern: two inputs use `left`/`right`, one
/// input uses `child`.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "db/common/plan.h"

namespace db
{

    /// JSON value type of plans. `ordered_json` keeps fields in the order
    /// they were written, so output is deterministic and reads like the spec.
    using PlanJson = nlohmann::ordered_json;

    /// The plan as JSON (see the field table above). Throws std::logic_error
    /// for a malformed node (wrong number of children, missing condition).
    PlanJson toJson(const PlanNode& plan);

    /// Reads a LOGICAL plan written by toJson, or by another module following
    /// the same field table. Only the logical ops are accepted: a physical
    /// name such as `hash_join` or `seqscan` is "Unknown plan operator
    /// 'hash_join'." (sql SPEC §M3.5: a logical plan never names a physical
    /// algorithm). The plan is checked before it is returned, and every
    /// problem names the offending node, e.g.
    /// `Node 'select': missing required field 'condition'.`:
    /// - Error(Schema): not an object, missing or unknown `op`, a missing,
    ///   unexpected or wrongly typed field (so `mode` is rejected), an empty
    ///   `table`/`to`/`columns`, a column string that is not `name` or
    ///   `qualifier.name`, sort `keys` empty or an entry that is not
    ///   [column, "asc"/"desc"], an aggregate that is not [function, column,
    ///   alias] (unknown function, `*` with a function other than count, an
    ///   alias that is not a bare name), limit `n` that is not a whole number
    ///   0..2^31-1, or a plan deeper than kMaxNestingDepth (the limits.h rule,
    ///   plan and condition levels together).
    /// - The condition's own category (Lexical or Syntax) for a `condition`
    ///   string that does not parse, with the node named in the message.
    /// Names are not resolved against any schema here (see outputSchema).
    PlanNode fromJson(const PlanJson& json);

    /// toJson as text: one line when `indent` is negative, else pretty-printed
    /// with `indent` spaces per level. JSON text must be valid UTF-8, so a
    /// plan holding a string that is not (e.g. a VARCHAR literal with a lone
    /// byte 0xC3) is Error(Schema); toJson itself accepts it.
    std::string toJsonText(const PlanNode& plan, int indent = 2);

    /// Parses JSON text and reads it with fromJson. Text that is not JSON is
    /// Error(Syntax) "Malformed plan JSON: ..." at its line and column.
    PlanNode fromJsonText(std::string_view text);

    /// Reads a PHYSICAL plan (exec SPEC §M2.2), with the same checks and
    /// messages as fromJson, but only physical ops are accepted: a logical op
    /// such as `scan` or `join` is "Unknown plan operator 'scan'.". Also
    /// Error(Schema), naming the node, for: a bad `mode`; sort `keys` empty
    /// or an entry that is not [column, "asc"/"desc"]; an aggregate that is
    /// not [function, column], an unknown function, or `*` with a function
    /// other than count; limit `n` that is not a whole number 0..2^31-1.
    /// Tables and columns are not looked up here; exec does that at open().
    PlanNode physicalFromJson(const PlanJson& json);

    /// Parses JSON text and reads it with physicalFromJson. Text that is not
    /// JSON is Error(Syntax) at its line and column, as in fromJsonText.
    PlanNode physicalFromJsonText(std::string_view text);

} // namespace db
