#pragma once

/// @file
/// Record id (§M1.2): "A record id (rid) is the pair (page id, slot number)".
/// A rid names one record of a table for as long as the record lives: slots
/// are never reused or renumbered (§M1.4), so a rid never changes.

#include <compare>
#include <cstdint>
#include <string>

#include "db/storage/page.h"

namespace db::storage
{
    struct Rid
    {
        PageId pageId = kInvalidPageId;
        std::uint16_t slot = 0;

        /// ==, <, <=, ... compare pageId first, then slot (the order the
        /// fields are declared). The ordering lets a Rid be a std::set or
        /// std::map key in tests and in exec.
        auto operator<=>(const Rid&) const = default;

        /// "(3, 7)": the form every error message uses to name a rid (§M1.8).
        std::string toString() const
        {
            return "(" + std::to_string(pageId) + ", " + std::to_string(slot) + ")";
        }
    };
} // namespace db::storage
