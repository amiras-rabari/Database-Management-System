#pragma once

/// @file
/// Turns parsed Bonus 1 relation text into typed relations (CLI only).

#include "relation_parser.h"

#include "db/common/schema.h"

#include <string>
#include <vector>

namespace ra
{
    /// A stored relation: its name, a schema whose columns are all qualified
    /// by that name, and its rows.
    struct Relation
    {
        std::string name;
        db::Schema schema;
        std::vector<db::Tuple> rows;
    };

    /// Types every parsed relation, in order. Bonus 1 relation text has no
    /// declared types, so they come from the data:
    /// - A column's type is that of its value in the FIRST row: an unquoted
    ///   integer is INT, a quoted or bare string is VARCHAR. A later row with
    ///   the other kind is Error(Type) "Type mismatch in relation ...".
    /// - Every unquoted integer must fit INT (int32), else Error(Lexical).
    /// - VARCHAR(n): n is the longest value of the column in bytes (at least
    ///   1); a value longer than 255 bytes is Error(Schema).
    /// - A relation without rows has every column VARCHAR(255).
    /// - A relation name defined twice is Error(Name); an attribute named
    ///   twice in one relation is Error(Schema).
    /// - Rows are a bag: duplicate rows are kept, in text order.
    std::vector<Relation> loadRelations(const std::vector<TextRelation>& relations);
}
