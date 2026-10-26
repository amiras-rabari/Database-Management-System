#pragma once

/// @file
/// Which replacement policy the buffer pool uses, chosen when a tool starts
/// (§M1.1: "Two replacement policies, LRU and CLOCK, selectable at startup";
/// §M1.9: `--policy lru|clock`). When `--policy` is not given, every tool
/// and the shell use LRU (DECISIONS.md 2026-10-25).

#include <optional>
#include <string_view>

namespace db::storage
{

    /// The policy that decides which frame the pool empties when it is full (§M1.6).
    /// An `enum class`, so the names stay inside ReplacementPolicy:: and the
    /// values never turn into plain ints by accident.
    enum class ReplacementPolicy
    {
        Lru,
        Clock
    };

    /// The policy's name exactly as `--policy` and the trace JSON
    /// `"policy"` field write it: "lru" or "clock".
    std::string_view policyName(ReplacementPolicy policy);

    /// Turns "lru" into Lru and "clock" into Clock (exact, lowercase).
    /// Anything else gives an empty optional. The CLI then reports a usage
    /// error; this library function never prints anything itself.
    std::optional<ReplacementPolicy> parsePolicy(std::string_view text);

} // namespace db::storage
