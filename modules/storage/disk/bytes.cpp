#include "db/storage/bytes.h"

#include <stdexcept>
#include <string>

namespace db::storage
{
    namespace
    {
        // Throws std::logic_error unless the `size` bytes starting at `offset`
        // lie inside a view of `viewSize` bytes.(offset + size > viewSize) looks right,
        // but it has a trap. size_t is unsigned and has a maximum then offset + size wraps around past
        // the maximum back to a tiny number hence safe version Checked as a subtraction so a
        // huge offset cannot wrap `offset + size` around to a small number.
        // viewSize - offset < size: how many bytes are left from offset to the end, and are there fewer
        // than we need? Because step 1 guaranteed offset ≤ viewSize, this subtraction can never go below zero,
        // so it can't wrap
        // Example: 4 bytes, readU16 at offset 3. Bytes left = 4 − 3 = 1, and we need 2. 1 < 2, so it throws
        // At offset 2: bytes left = 2, which isn't < 2, so it's OK
        void checkRange(std::size_t viewSize, std::size_t offset, std::size_t size, const char* function)
        {
            if (offset > viewSize || viewSize - offset < size)
                throw std::logic_error(std::string(function) + ": offset " + std::to_string(offset) + " + "
                    + std::to_string(size) + " is outside a view of "
                    + std::to_string(viewSize) + " bytes");
        }
    } // namespace

    // Byte 0 holds the lowest 8 bits. Assembling with shifts gives the same
    // result on any host, so no "is this machine little-endian?" check and no
    // cast of a byte pointer to an integer pointer (misaligned access and a
    // strict-aliasing violation, both undefined behaviour).
    std::uint16_t readU16(std::span<const std::byte> bytes, std::size_t offset)
    {
        checkRange(bytes.size(), offset, 2, "readU16");
        //  C++ silently promotes small types like uint16_t to int before doing
        //  arithmetic on them so we apply the static cast here same idea in other functions where applicable
        return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset])
            | std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8);
    }

    std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset)
    {
        checkRange(bytes.size(), offset, 4, "readU32");
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i)
            // cast to std::uint32_t> so shift is well defined and  does not do sign extensions
            // std::to_integer<> is template that tell compiler to convert to specific type of
            // int defined inside <>
            value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
        return value;
    }

    //  uint32 -> int32 conversion is defined as two's
    // complement, which is exactly the INT encoding of §M1.3.
    std::int32_t readI32(std::span<const std::byte> bytes, std::size_t offset)
    {
        checkRange(bytes.size(), offset, 4, "readI32");
        return static_cast<std::int32_t>(readU32(bytes, offset));
    }

    // Same loop as readU32, over 8 bytes. Each byte is widened to uint64_t
    // before the shift: shifting a 32-bit value by 32 or more is undefined.
    std::uint64_t readU64(std::span<const std::byte> bytes, std::size_t offset)
    {
        checkRange(bytes.size(), offset, 8, "readU64");
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i)
            value |= std::to_integer<std::uint64_t>(bytes[offset + i]) << (8 * i);
        return value;
    }

    void writeU16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value)
    {
        checkRange(bytes.size(), offset, 2, "writeU16");
        bytes[offset] = static_cast<std::byte>(value & 0xFF);
        bytes[offset + 1] = static_cast<std::byte>(value >> 8);
    }

    void writeU32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value)
    {
        checkRange(bytes.size(), offset, 4, "writeU32");
        for (std::size_t i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }

    void writeI32(std::span<std::byte> bytes, std::size_t offset, std::int32_t value)
    {
        checkRange(bytes.size(), offset, 4, "writeI32");
        writeU32(bytes, offset, static_cast<std::uint32_t>(value));
    }

    void writeU64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value)
    {
        checkRange(bytes.size(), offset, 8, "writeU64");
        for (std::size_t i = 0; i < 8; ++i)
            bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }
} // namespace db::storage
