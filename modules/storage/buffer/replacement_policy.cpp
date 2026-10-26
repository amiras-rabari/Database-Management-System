#include "db/storage/replacement_policy.h"

#include <stdexcept>

namespace db::storage
{

    std::string_view policyName(ReplacementPolicy policy)
    {
        // There is no `default:` on purpose: if a new policy is added to the
        // enum, the compiler warns that this switch does not handle it.
        switch (policy)
        {
        case ReplacementPolicy::Lru:
            return "lru";
        case ReplacementPolicy::Clock:
            return "clock";
        }
        // Only reachable if someone cast a bad number into the enum,
        // e.g. static_cast<ReplacementPolicy>(7). That is a bug (D1).
        throw std::logic_error("policyName: invalid ReplacementPolicy value");
    }

    std::optional<ReplacementPolicy> parsePolicy(std::string_view text)
    {
        // Exact, lowercase match only: the names used by `--policy` and the
        // trace JSON (§M1.9). So "LRU" or "Clock" give nothing, and the CLI
        // reports them as a usage error.
        if (text == "lru")
            return ReplacementPolicy::Lru;
        if (text == "clock")
            return ReplacementPolicy::Clock;
        return std::nullopt;
    }

} // namespace db::storage
