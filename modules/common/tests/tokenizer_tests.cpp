// Tokenizer regression tests.

#include <stdexcept>
#include <string>

#include <doctest/doctest.h>

#include "relation_lexer.h"

namespace {

std::size_t tokenCount(const std::string& input) {
    return ra::Tokenizer(input).tokenizeAll().size();
}

}  // namespace

// I-1: inside { } a \v or \f used to stop a value without being consumed, so
// the tokenizer emitted empty values forever. The ctest TIMEOUT turns a
// regression into a failure instead of a hang.
TEST_CASE("vertical tab and form feed inside relation data are whitespace") {
    // R ( a ) = { 1 2 } EOF
    CHECK(tokenCount("R (a) = {\v1\f2\v}") == 10);
    CHECK(tokenCount("R (a) = { 1 \v\f }") == 9);
}

TEST_CASE("vertical tab and form feed in a query are whitespace") {
    CHECK(tokenCount("select[a=1]\v(\fR)") == tokenCount("select[a=1] ( R)"));
}
