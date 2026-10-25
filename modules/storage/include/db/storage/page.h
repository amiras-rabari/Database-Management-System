#pragma once

/// @file
/// Page constants shared by every on-disk layout of the storage module
/// (SPEC §M1.2): fixed 4096-byte pages addressed by a zero-based page id.

#include <cstddef>
#include <cstdint>

namespace db::storage
{

    /// Size of every page in every file, in bytes.
    /// NOTE : used inline here so Prevents multiple-definition errors across files
    /// and hints at code optimization. As our PageSize is fixed at 4096 bytes for all pages
    inline constexpr std::size_t kPageSize = 4096;

    /// A simple alias for easy use
    /// Zero-based index of a page inside its file. Stored on disk as uint32.
    using PageId = std::uint32_t;

    /// "No page", e.g. `next_dir_page` of the last directory page (§M1.5).
    /// Never a valid page id, so a file holds at most kInvalidPageId pages.
    inline constexpr PageId kInvalidPageId = 0xFFFFFFFF;

} // namespace db::storage
