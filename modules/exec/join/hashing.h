#pragma once

// Our own deterministic hash (chunk 09, DEPENDENCIES G4). Used to split
// tuples into partitions (h1) and to find them in an in-memory table (h2).
// Private to exec.
//
// Why not std::hash: the C++ standard does not fix its values, so MinGW on
// Windows and g++ on Linux may give different numbers, and then partition
// sizes, overflow and counters would differ between machines (§0.2: same
// data + plan + B gives the same counters). libstdc++ also hashes an int as
// itself, which spreads patterned keys badly under "mod P".

#include <cstddef>
#include <cstdint>
#include <vector>

#include "db/common/schema.h"

namespace db::exec
{
    /// FNV-1a 64-bit: the start value and the multiplier (published constants).
    inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    /// The seeds of the hash levels. Different seeds matter: if the table
    /// used the partitioner's hash, all keys of partition i (same h1 mod P)
    /// would crowd into a few buckets.
    inline constexpr std::uint64_t kSeedPartition = 1; // h1: which partition
    inline constexpr std::uint64_t kSeedTable = 2; // h2: which bucket of a table

    /// One FNV-1a step: mixes one byte into `hash`.
    std::uint64_t fnv1aAddByte(std::uint64_t hash, std::uint8_t byte);

    /// Hash of the values at `keyColumns` of `tuple`, with `seed`.
    /// The bytes hashed are the record codec's layout of each value (§M1.3):
    /// INT = 4 bytes little-endian, VARCHAR = 2-byte length little-endian
    /// then its bytes. They are built with shifts, never by reading a
    /// value's memory, so the result is the same on every platform. Equal
    /// keys give equal hashes (types are fixed per column, no NULLs).
    std::uint64_t hashKey(const Tuple& tuple, const std::vector<std::size_t>& keyColumns, std::uint64_t seed);
} // namespace db::exec
