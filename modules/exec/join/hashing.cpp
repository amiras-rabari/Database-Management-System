#include "join/hashing.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "db/common/schema.h"
#include "db/common/value.h"

namespace db::exec
{
    namespace
    {
        // Mixes the low `count` bytes of `bits` into `hash`, lowest byte
        // first (little-endian). Example: 42 as 4 bytes is 2A 00 00 00.
        std::uint64_t addLittleEndian(std::uint64_t hash, std::uint64_t bits, int count)
        {
            for (int i = 0; i < count; ++i)
                hash = fnv1aAddByte(hash, static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
            return hash;
        }

        // The splitmix64 finaliser: spreads every input bit over all 64
        // output bits. FNV-1a alone changes the low bits little for keys
        // that differ only in their high bits, and "mod P" uses the low bits.
        std::uint64_t finalMix(std::uint64_t x)
        {
            x ^= x >> 30;
            x *= 0xBF58476D1CE4E5B9ULL;
            x ^= x >> 27;
            x *= 0x94D049BB133111EBULL;
            x ^= x >> 31;
            return x;
        }
    } // namespace

    std::uint64_t fnv1aAddByte(std::uint64_t hash, std::uint8_t byte)
    {
        // FNV-1a: first xor the byte in, then multiply (unsigned, so it
        // wraps around mod 2^64 the same way on every platform).
        hash ^= byte;
        hash *= kFnvPrime;
        return hash;
    }

    std::uint64_t hashKey(const Tuple& tuple, const std::vector<std::size_t>& keyColumns, std::uint64_t seed)
    {
        // The seed goes in first (8 bytes), so each seed gives a different
        // hash function.
        std::uint64_t hash = addLittleEndian(kFnvOffsetBasis, seed, 8);
        for (std::size_t column : keyColumns)
        {
            const Value& value = tuple[column];
            if (value.type() == Type::Int)
            {
                // Two's complement bits of the INT, as the record codec stores it.
                hash = addLittleEndian(hash, static_cast<std::uint32_t>(value.asInt()), 4);
            }
            else
            {
                const std::string& text = value.asVarchar();
                hash = addLittleEndian(hash, text.size(), 2); // at most 255 bytes
                for (char c : text)
                    hash = fnv1aAddByte(hash, static_cast<std::uint8_t>(c));
            }
        }
        return finalMix(hash);
    }
} // namespace db::exec
