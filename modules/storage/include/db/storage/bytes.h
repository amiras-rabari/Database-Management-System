#pragma once

/// @file
/// Little-endian integer helpers over raw byte views (SPEC M1.2: "All
/// multi-byte integers are little-endian"). Every page layout reads and writes
/// its fields through these, never through pointer casts.
/// The _t types with a number in the name (uint16_t, int32_t and so on) are the fixed-width types.
/// We use them for everything that goes to disk, because the file has to look the same on Windows and Linux.

#include <cstddef>
#include <cstdint>
#include <span>

namespace db::storage
{

    /// A span does not allocate, copy, or free any data. It simply points to data managed by another container
    /// It only works with structures where elements sit next to each other in memory

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
