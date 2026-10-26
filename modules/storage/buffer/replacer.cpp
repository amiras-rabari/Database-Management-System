#include "buffer/replacer.h"

#include <stdexcept>

namespace db::storage
{

    std::unique_ptr<Replacer> makeReplacer(ReplacementPolicy policy, std::size_t frames)
    {
        // Build the policy that was asked for. Each branch makes an
        // LruReplacer or ClockReplacer, and the returned pointer is
        // converted to a unique_ptr<Replacer> automatically.
        // The constructors check that frames > 0.
        // There is no `default:` on purpose: if a new policy is added to the
        // enum, the compiler warns that this switch does not handle it.
        switch (policy)
        {
        case ReplacementPolicy::Lru:
            return std::make_unique<LruReplacer>(frames);
        case ReplacementPolicy::Clock:
            return std::make_unique<ClockReplacer>(frames);
        }
        // Only reachable if someone cast a bad number into the enum,
        // e.g. static_cast<ReplacementPolicy>(7). That is a bug (D1).
        throw std::logic_error("makeReplacer: invalid ReplacementPolicy value");
    }

} // namespace db::storage
