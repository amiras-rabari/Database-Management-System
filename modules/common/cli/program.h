#pragma once

/// @file
/// Reads one Bonus 1 input text: relation definitions followed by a query.
/// The relation-text format belongs to the CLI, not to db_common; the query
/// goes to the library's parser.

#include "relation_loader.h"

#include "db/common/plan.h"

#include <string>
#include <vector>

namespace ra
{
    /// One parsed Bonus 1 input.
    struct Program
    {
        /// Relations defined before the query; empty in tree-only mode.
        std::vector<Relation> relations;
        /// The query that follows the relations.
        db::PlanNode query;
    };

    /// Parses `text`. In tree-only mode the whole text is a query; otherwise
    /// one or more relation definitions come first (see loadRelations for
    /// their typing). The query part is parsed by db::parseRaQuery on the
    /// original text with every relation character except newlines replaced
    /// by a space, so query error positions are real lines and columns.
    /// Throws db::Error.
    Program parseProgram(const std::string& text, bool treeOnlyMode);
}
