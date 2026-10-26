#include "db/storage/replacement_policy.h"

#include <stdexcept>

namespace db::storage
{

    std::string_view policyName(ReplacementPolicy policy)
    {
        // No default case: if a policy is ever added, -Wswitch warns here.
        switch (policy)
        {
        case ReplacementPolicy::Lru:
            return "lru";
        case ReplacementPolicy::Clock:
            return "clock";
        }
        // Reached only through a cast of an out-of-range integer (caller bug, D1).
        throw std::logic_error("policyName: invalid ReplacementPolicy value");
    }

    std::optional<ReplacementPolicy> parsePolicy(std::string_view text)
    {
        // Exact, case-sensitive match: the CLI and trace JSON names (§M1.9).
        if (text == "lru")
            return ReplacementPolicy::Lru;
        if (text == "clock")
            return ReplacementPolicy::Clock;
        return std::nullopt;
    }

} // namespace db::storage
