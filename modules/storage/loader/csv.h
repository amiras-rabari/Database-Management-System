#pragma once

/// @file
/// Private to storage (chunk 13): splits one CSV line into its fields with a
/// small hand-written state machine (the hard rule forbids <regex>).
///
/// The dialect (STATUS 2026-10-27, chunks 13–15 row):
/// - fields are separated by commas; there is no header line;
/// - a field that starts with `"` is quoted and ends at the matching `"`;
///   inside quotes a comma is ordinary text and `""` stands for one `"`;
/// - a `"` in the middle of an unquoted field is ordinary text;
/// - spaces are data (nothing is trimmed);
/// - one trailing `\r` is removed first (Windows line endings);
/// - an empty line has zero fields (so the loader reports it as a wrong
///   field count);
/// - a quoted field cannot span lines.

#include <string>
#include <string_view>
#include <vector>

namespace db::storage
{
    /// The fields of `line` (without its `\n`), in order. Each field owns its
    /// text, so it survives when the caller reuses the line buffer.
    ///
    /// Errors: db::Error(Syntax) with line() = 0 (the caller knows the line
    /// number) and column() = the 1-based number of the field, for
    /// - "unterminated quoted field" (end of line inside quotes);
    /// - "text after the closing quote" (anything but `,` right after it).
    std::vector<std::string> splitCsvLine(std::string_view line);
} // namespace db::storage
