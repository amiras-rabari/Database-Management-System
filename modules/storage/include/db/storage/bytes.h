#pragma once

/// @file
/// Little-endian integer helpers over raw byte views (SPEC §M1.2: "All
/// multi-byte integers are little-endian"). Every page layout reads and writes
/// its fields through these, never through pointer casts.

#include <cstddef>
#include <cstdint>
#include <span>

namespace db::storage
{

    /// Reads the unsigned 16-bit integer stored little-endian at `offset`.
    /// Throws std::logic_error if `offset + 2` is past the end of `bytes`.
    std::uint16_t readU16(std::span<const std::byte> bytes, std::size_t offset);
    /// Reads the unsigned 32-bit integer stored little-endian at `offset`.
    std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset);
    /// Reads the signed (two's complement) 32-bit integer at `offset`.
    std::int32_t readI32(std::span<const std::byte> bytes, std::size_t offset);

    /// Writes `value` little-endian at `offset` (low byte first).
    /// Throws std::logic_error if `offset + 2` is past the end of `bytes`.
    void writeU16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value);
    /// Writes `value` little-endian at `offset`.
    void writeU32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value);
    /// Writes `value` as 4 bytes of two's complement, little-endian.
    void writeI32(std::span<std::byte> bytes, std::size_t offset, std::int32_t value);

} // namespace db::storage
