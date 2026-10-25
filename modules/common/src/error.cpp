#include "db/common/error.h"

#include <utility>

namespace db
{
    // Anonymous namespace: Think of this as a "private static" area in Java.
    // Functions declared here (like formatParts) are strictly locked to this file.
    // This prevents name clashes at the linker stage if another file uses the same name.
    // So this helper method can only be called within this file.
    namespace
    {

        std::string formatParts(ErrorCategory category, const std::string& message, int line, int column)
        {
            std::string text = "error: ";
            text += categoryName(category);
            text += ": ";
            text += message;
            if (line != 0)
            {
                text += " at line " + std::to_string(line) + ", column " + std::to_string(column);
            }
            return text;
        }

    } // namespace

    std::string_view categoryName(ErrorCategory category)
    {
        switch (category)
        {
            case ErrorCategory::Lexical:
                return "lexical";
            case ErrorCategory::Syntax:
                return "syntax";
            case ErrorCategory::Name:
                return "name";
            case ErrorCategory::Ambiguity:
                return "ambiguity";
            case ErrorCategory::Type:
                return "type";
            case ErrorCategory::Aggregate:
                return "aggregate";
            case ErrorCategory::Schema:
                return "schema";
        }
        throw std::logic_error("categoryName: unknown ErrorCategory value");
    }

    // The base class is built from the formatted text so that what() needs no
    // override and stays valid for the lifetime of the exception.
    Error::Error(ErrorCategory category, std::string message, int line, int column)
        : std::runtime_error(formatParts(category, message, line, column)),
          category_(category),
          message_(std::move(message)),
          line_(line),
          column_(column)
    {
    }

    ErrorCategory Error::category() const
    {
        return category_;
    }

    int Error::line() const
    {
        return line_;
    }

    int Error::column() const
    {
        return column_;
    }

    const std::string& Error::message() const
    {
        return message_;
    }

    std::string formatError(const Error& error)
    {
        return formatParts(error.category(), error.message(), error.line(), error.column());
    }

} // namespace db
