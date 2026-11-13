#include "db/opt/explain.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "db/common/condition.h"
#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/opt/enumerate.h"
#include "db/opt/estimate.h"
#include "db/opt/physical.h"

namespace db::opt
{
    namespace
    {
        // ---- explain ------------------------------------------------------

        // `R.a, R.b`: the columns separated by ", ".
        std::string columnList(const std::vector<ColumnRef>& columns)
        {
            std::string text;
            for (std::size_t i = 0; i < columns.size(); ++i)
            {
                text += (i == 0 ? "" : ", ") + columns[i].toString();
            }
            return text;
        }

        // `count(*)`, `avg(R.salary)`; a logical aggregate adds its output
        // name: `count(*) as n` (a physical one has no alias).
        std::string aggregateText(const Aggregate& aggregate)
        {
            std::string text = std::string(aggregateFunctionName(aggregate.function)) + "(";
            text += aggregate.column ? aggregate.column->toString() : "*";
            text += ")";
            if (!aggregate.alias.empty())
            {
                text += " as " + aggregate.alias;
            }
            return text;
        }

        // ` mode=sort` or ` mode=hash` for a physical distinct/group_by;
        // nothing for a logical one (the optimizer has not chosen yet).
        std::string modeText(const PlanNode& node)
        {
            if (!node.mode)
            {
                return "";
            }
            return *node.mode == ExecMode::Sort ? " mode=sort" : " mode=hash";
        }

        // The text that names one node: its op name as plan JSON writes it,
        // then what it works on. The condition text is common's toString,
        // the same text the plan JSON holds.
        std::string nodeLabel(const PlanNode& node)
        {
            std::string label(opName(node.op));
            switch (node.op)
            {
            case PlanOp::Scan:
            case PlanOp::SeqScan:
                return label + " " + node.table;
            case PlanOp::Select:
            case PlanOp::Filter:
            case PlanOp::Join:
            case PlanOp::NestedLoopJoin:
            case PlanOp::SortMergeJoin:
            case PlanOp::HashJoin:
                return label + " (" + toString(*node.condition) + ")";
            case PlanOp::Project:
                return label + " [" + columnList(node.columns) + "]";
            case PlanOp::Rename:
                return label + " " + node.alias;
            case PlanOp::Sort:
            {
                std::string keys;
                for (std::size_t i = 0; i < node.sortKeys.size(); ++i)
                {
                    const SortKey& key = node.sortKeys[i];
                    keys += (i == 0 ? "" : ", ") + key.column.toString() + (key.ascending ? " asc" : " desc");
                }
                return label + " [" + keys + "]";
            }
            case PlanOp::GroupBy:
            {
                // `[keys; aggregates]`, like common's one-line plan text;
                // with no keys just `[aggregates]`.
                std::string inside = columnList(node.columns);
                for (std::size_t i = 0; i < node.aggregates.size(); ++i)
                {
                    std::string separator = i == 0 ? (inside.empty() ? "" : "; ") : ", ";
                    inside += separator + aggregateText(node.aggregates[i]);
                }
                return label + " [" + inside + "]" + modeText(node);
            }
            case PlanOp::Distinct:
                return label + modeText(node);
            case PlanOp::Limit:
                return label + " " + std::to_string(node.limitCount);
            default:
                // cross, union, union_all, intersect, except: the name says it all.
                return label;
            }
        }

        // Adds the label of `node` and of every node below it, in pre-order,
        // each indented 2 spaces per level. The label is built in nodeLabel,
        // which returns before the recursion goes on, so a frame here stays
        // small and a tree 1000 levels deep fits on the stack.
        void collectLabels(const PlanNode& node, std::size_t depth, std::vector<std::string>& labels)
        {
            labels.push_back(std::string(2 * depth, ' ') + nodeLabel(node));
            for (const PlanNode& child : node.children)
            {
                collectLabels(child, depth + 1, labels);
            }
        }

        // ---- DP table -----------------------------------------------------

        // True when relation i is in `group` (its bit is 1).
        bool contains(std::uint32_t group, std::size_t i)
        {
            return (group & (1u << i)) != 0;
        }

        // `{A,B'}`: the relations of `group`, in index order.
        std::string subsetText(std::uint32_t group, const std::vector<std::string>& names)
        {
            std::string text = "{";
            bool first = true;
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (contains(group, i))
                {
                    text += (first ? "" : ",") + names[i];
                    first = false;
                }
            }
            return text + "}";
        }

        // The name of the one relation in a single-relation `group`.
        std::string singleName(std::uint32_t group, const std::vector<std::string>& names)
        {
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (contains(group, i))
                {
                    return names[i];
                }
            }
            return "";
        }

        // How many relations are in `group`.
        std::size_t memberCount(std::uint32_t group, std::size_t relationCount)
        {
            std::size_t count = 0;
            for (std::size_t i = 0; i < relationCount; ++i)
            {
                if (contains(group, i))
                {
                    ++count;
                }
            }
            return count;
        }

        std::string algorithmText(JoinAlgorithm algorithm)
        {
            switch (algorithm)
            {
            case JoinAlgorithm::NestedLoop:
                return "BNL";
            case JoinAlgorithm::SortMerge:
                return "sort-merge";
            default:
                return "hash";
            }
        }

        // `B' outer, BNL with A inner`. A joined outer also shows its
        // estimated pages, the M of the formula, as §M4.5 does:
        // `{A,B'} outer (20 pages), BNL with C inner`.
        std::string candidateText(const DpTable& table, const DpCandidate& candidate)
        {
            std::string outer;
            if (memberCount(candidate.outer, table.names.size()) == 1)
            {
                outer = singleName(candidate.outer, table.names) + " outer";
            }
            else
            {
                std::int64_t pages = 0;
                for (const DpEntry& entry : table.entries)
                {
                    if (entry.subset == candidate.outer)
                    {
                        pages = entry.pages;
                    }
                }
                outer = subsetText(candidate.outer, table.names) + " outer (" + std::to_string(pages) + " pages)";
            }
            return outer + ", " + algorithmText(candidate.algorithm) + " with " + table.names[candidate.inner] +
                   " inner";
        }

        // One Markdown row. An empty subset cell prints as `| |`, as in the
        // spec's alternative row.
        std::string row(const std::string& subset, const std::string& plan, const std::string& cost)
        {
            std::string subsetCell = subset.empty() ? "| " : "| " + subset + " ";
            return subsetCell + "| " + plan + " | " + cost + " |\n";
        }
    } // namespace

    std::string explainText(const PlanNode& plan, const std::vector<NodeEstimate>& estimates, bool showCost)
    {
        // First build every label and every `est_rows=N`, then measure the
        // longest of each, then write the lines: the columns line up only
        // once all lines are known.
        std::vector<std::string> labels;
        collectLabels(plan, 0, labels);
        std::vector<std::string> rowsTexts;
        std::size_t labelWidth = 0;
        std::size_t rowsWidth = 0;
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            rowsTexts.push_back("est_rows=" + std::to_string(estimates[i].rows));
            labelWidth = std::max(labelWidth, labels[i].size());
            rowsWidth = std::max(rowsWidth, rowsTexts[i].size());
        }

        // §M4.6's example: `est_rows=` 8 spaces after the longest label,
        // `est_cost=` 2 spaces after the longest `est_rows=N`.
        std::string text;
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            text += labels[i] + std::string(labelWidth - labels[i].size() + 8, ' ') + rowsTexts[i];
            if (showCost)
            {
                text += std::string(rowsWidth - rowsTexts[i].size() + 2, ' ') +
                        "est_cost=" + std::to_string(estimates[i].cost);
            }
            text += "\n";
        }
        return text;
    }

    std::string dpTableText(const DpTable& table)
    {
        std::string text = "Join block: ";
        for (std::size_t i = 0; i < table.names.size(); ++i)
        {
            text += (i == 0 ? "" : ", ") + table.names[i];
        }
        text += "\n\n";
        if (table.entries.empty())
        {
            return text + "More than six relations: no DP; the joins keep their written order.\n";
        }

        text += "| Subset | Best plan considered | Cost |\n|---|---|---|\n";
        for (const DpEntry& entry : table.entries)
        {
            std::string subset = subsetText(entry.subset, table.names);
            if (entry.skipped)
            {
                text += row(subset, "no predicate, skipped", "—");
            }
            else if (entry.candidates.empty())
            {
                // A single relation: its access path.
                text += row(subset, "scan " + singleName(entry.subset, table.names), std::to_string(entry.cost));
            }
            else
            {
                text += row(subset, candidateText(table, entry.candidates[entry.chosen]), std::to_string(entry.cost));
            }
        }

        // The group of all relations is the last entry. For every other
        // relation that could have been joined last, show its cheapest way
        // (the first one tried on a tie), as §M4.5's "alternative" row.
        const DpEntry& all = table.entries.back();
        if (all.candidates.empty())
        {
            return text;
        }
        std::size_t chosenInner = all.candidates[all.chosen].inner;
        for (std::size_t inner = 0; inner < table.names.size(); ++inner)
        {
            if (inner == chosenInner)
            {
                continue;
            }
            const DpCandidate* best = nullptr;
            for (const DpCandidate& candidate : all.candidates)
            {
                if (candidate.inner == inner && (best == nullptr || candidate.cost < best->cost))
                {
                    best = &candidate;
                }
            }
            if (best != nullptr)
            {
                text += row("", "alternative: " + candidateText(table, *best), std::to_string(best->cost));
            }
        }
        return text;
    }
} // namespace db::opt
