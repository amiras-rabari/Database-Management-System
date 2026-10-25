#include "program.h"

#include "parser_context.h"
#include "relation_lexer.h"
#include "relation_parser.h"

#include "db/common/ra_parser.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ra
{
    namespace
    {
        // The query text with the relation part (everything before
        // `queryStart`) blanked out. Newlines stay, so positions match.
        std::string queryOnly(const std::string& text, std::size_t queryStart)
        {
            std::string query = text;
            for (std::size_t i = 0; i < queryStart; ++i)
            {
                if (query[i] != '\n')
                {
                    query[i] = ' ';
                }
            }
            return query;
        }
    }

    Program parseProgram(const std::string& text, bool treeOnlyMode)
    {
        Program program;
        if (treeOnlyMode)
        {
            program.query = db::parseRaQuery(text);
            return program;
        }

        Tokenizer lexer(text);
        ParserContext context(lexer);
        const std::vector<TextRelation> relations = RelationParser(context).parseRelationFile();
        // The first token the relation parser did not use starts the query.
        const std::size_t queryStart = context.peek().offset;

        // Both texts are parsed before the relations are typed, the order in
        // which Bonus 1 reported errors.
        program.query = db::parseRaQuery(queryOnly(text, queryStart));
        program.relations = loadRelations(relations);
        return program;
    }
}
