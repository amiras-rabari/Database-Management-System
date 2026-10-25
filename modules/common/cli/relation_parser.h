#pragma once

/// @file
/// Parser for Bonus 1 relation text (`Emp (EID, Name) = { E1, Alice }`), CLI only.
/// It checks the text's shape and row widths; relation_loader.h gives the
/// values their types.

#include "parser_context.h"

#include <string>
#include <vector>

namespace ra
{
    /// How a value was written: an unquoted integer, or a string (quoted or
    /// bare; the quotes are only spelling).
    enum class TextValueKind
    {
        Integer,
        String
    };

    /// One value as written in the relation text.
    struct TextValue
    {
        TextValueKind kind;
        /// The integer as written, or the decoded string.
        std::string text;
        int line;
        int column;
    };

    /// One row of values, as written.
    struct TextRow
    {
        std::vector<TextValue> values;
    };

    /// One relation definition, as written.
    struct TextRelation
    {
        std::string name;
        /// Position of the relation name.
        int line = 0;
        int column = 0;
        std::vector<std::string> attributes;
        std::vector<TextRow> rows;
    };

    class RelationParser
    {
    public:
        explicit RelationParser(ParserContext& context);

        /// Parses one or more relation definitions and stops at the first
        /// token that does not start another one (the query). Throws
        /// db::Error: Lexical, Syntax, or Schema for a row whose value count
        /// differs from the attribute count.
        std::vector<TextRelation> parseRelationFile();

    private:
        ParserContext& context;

        // ------------------------------------------------------------
        // Relation grammar
        // ------------------------------------------------------------

        TextRelation parseRelationDefinition();
        TextRow parseRelationRow();
        TextValue parseValue();

        // Validates what comes after a tuple_line.
        //
        // Returns true if the relation body is closed by '}'.
        // Returns false if a valid row_separator was consumed and
        // another tuple row should follow.
        bool parseRelationRowEnd();

        // ------------------------------------------------------------
        // Relation-specific checks/helpers
        // ------------------------------------------------------------

        // Exact beginning of a relation:
        //
        //     IDENTIFIER LPAREN
        //
        bool isRelationDefinitionStart() const;

        bool isUnnamedRelationDefinitionStart() const;
        // value ::= integer | quoted_string | bare_string
        bool isValueStart() const;

        // Formatting newlines are allowed around structural syntax.
        void skipNewlines();

        // Used only while parsing a tuple.
        //
        // If the current token is a NEWLINE, determine whether,
        // after some number of NEWLINEs, a COMMA appears.
        //
        // This is necessary because:
        //
        //     1,\n25      -> same tuple
        //
        // while:
        //
        //     1\n25       -> two tuples
        //
        bool hasCommaAfterNewlines() const;
    };
}
