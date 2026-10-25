#pragma once

/// @file
/// The one exception type of the project, its error categories and the text
/// format every module prints errors in.

#include <stdexcept>
#include <string>
#include <string_view>

namespace db
{

    /// What kind of mistake an error reports. One list for the whole project.
    enum class ErrorCategory
    {
        Lexical,
        Syntax,
        Name,
        Ambiguity,
        Type,
        Aggregate,
        Schema
    };

    /// Lowercase name of a category as it appears in error text,
    /// e.g. `ambiguity`.
    std::string_view categoryName(ErrorCategory category);

    /// Error thrown by library code for a mistake in the user's input
    /// (query text, plan, data). Internal bugs use std::logic_error instead.
    ///
    /// `what()` returns the full formatted text (see formatError).
    /// public inheritance means Anything that was public in the base class (std::runtime_error) remains public
    /// in our custom class (Error) hence we can use what() directly that Error is a subclass of std::runtime_error.
    ///by doing public we get all the public methods of std::runtime_error so any class that inherits
    ///from Error can use them publicly as well if it was private then only Error class can use it.
    ///in java all is by default public inheritance.
    class Error : public std::runtime_error
    {
    public:
        /// Creates an error. `line` and `column` are 1-based; a `line` of 0
        /// means the error has no position in the input.
        Error(ErrorCategory category, std::string message, int line = 0, int column = 0);

        //- These are getters, the same as in Java.
        /// The kind of error.
        ErrorCategory category() const;
        /// 1-based line of the error, or 0 when there is no position.
        int line() const;
        /// 1-based column of the error; meaningful only when line() is not 0.
        int column() const;
        /// The message alone, without the `error: <category>: ` prefix or the
        /// position suffix.
        const std::string& message() const;

    private:
        //The trailing _ is the project's convention for member variables
        ErrorCategory category_;
        std::string message_;
        int line_;
        int column_;
    };

    /// Formats an error as `error: <category>: <message> at line L, column C`,
    /// or `error: <category>: <message>` when the error has no position.
    std::string formatError(const Error& error);

} // namespace db
