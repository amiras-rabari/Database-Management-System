#pragma once

/// @file
/// The buffer pool's replacement policy, chosen at startup (§M1.1: "Two
/// replacement policies, LRU and CLOCK, selectable at startup"; §M1.9
/// `--policy lru|clock`). The default everywhere is LRU.

#include <optional>
#include <string_view>

namespace db::storage
{

    /// Which frame the buffer pool reuses when it is full (§M1.6).
    /// A scoped enum: the values do not leak into the namespace and never
    /// convert to int by accident.
    enum class ReplacementPolicy
    {
        Lru,
        Clock
    };

    /// The policy's name exactly as the CLI flag and the trace JSON
    /// `"policy"` field write it: `lru` or `clock`.
    std::string_view policyName(ReplacementPolicy policy);

    /// `lru` -> Lru, `clock` -> Clock (exact, lowercase). Anything else gives
    /// an empty optional; the CLI turns that into a usage error (the library
    /// never prints).
    std::optional<ReplacementPolicy> parsePolicy(std::string_view text);

} // namespace db::storage
