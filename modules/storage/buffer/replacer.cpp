#include "buffer/replacer.h"

#include <stdexcept>

namespace db::storage
{

    std::unique_ptr<Replacer> makeReplacer(ReplacementPolicy policy, std::size_t frames)
    {
        // The constructors check frames > 0. Each branch converts the
        // derived unique_ptr to unique_ptr<Replacer>; the virtual destructor
        // makes deleting it through the base pointer safe.
        switch (policy)
        {
        case ReplacementPolicy::Lru:
            return std::make_unique<LruReplacer>(frames);
        case ReplacementPolicy::Clock:
            return std::make_unique<ClockReplacer>(frames);
        }
        // Reached only through a cast of an out-of-range integer (caller bug, D1).
        throw std::logic_error("makeReplacer: invalid ReplacementPolicy value");
    }

} // namespace db::storage
