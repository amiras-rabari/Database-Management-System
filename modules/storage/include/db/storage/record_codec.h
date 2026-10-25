#pragma once

/// @file
/// Turns a Tuple into the exact bytes of §M1.3 and back, and computes a
/// tuple's encoded length without building the bytes. Pure functions: no
/// files, no pages. Used by heap files, exec run files, the catalog and opt.
///
/// Record format (§M1.3): the fields in schema order, no header, no null
/// bitmap. INT = 4 bytes, two's complement, little-endian. VARCHAR = 2-byte
/// unsigned length L (in bytes, not characters), then the L bytes.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "db/common/schema.h"

namespace db::storage
{
    /// Number of bytes encodeRecord(tuple, schema) would produce: 4 per INT,
    /// 2 + byte length per VARCHAR. Builds nothing. Throws the same errors as
    /// encodeRecord, so a length is only ever computed for a valid tuple.
    std::uint32_t encodedLength(const Tuple& tuple, const Schema& schema);

    /// Encodes `tuple` as one record. Throws db::Error(Schema) if the tuple
    /// has the wrong number of values or a VARCHAR is longer than its
    /// column's n bytes, db::Error(Type) if a value's type differs from its
    /// column's type. VARCHAR bytes are stored as given (no UTF-8 check).
    std::vector<std::byte> encodeRecord(const Tuple& tuple, const Schema& schema);

    /// Decodes one record (for example the bytes of a slot) back into a tuple.
    /// The tuple owns its strings: they are copied out of `bytes`, because
    /// those bytes usually live in a buffer-pool frame that is reused as soon
    /// as the page is unpinned. Throws std::logic_error if the bytes end
    /// before the last column or bytes are left over (corrupt record).
    Tuple decodeRecord(std::span<const std::byte> bytes, const Schema& schema);
} // namespace db::storage
