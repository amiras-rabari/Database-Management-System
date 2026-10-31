#pragma once

/// @file
/// runTrace (chunk 15): replays a list of page ids through a fresh buffer
/// pool, for the `storage trace` command ("the `trace` command is how I
/// grade your buffer pool", §M1.9). DECISIONS.md 2026-10-27 "Trace replay".
///
/// The pool needs real pages to read, so runTrace makes a scratch file
/// `<dir>/tmp/trace_scratch.db` with max id + 1 zero pages (allocated with
/// DiskManager::allocatePage, which counts nothing, D2). Then, in order, it
/// fetches each page id and unpins it at once as clean (§M1.9). Every miss
/// is a real DiskManager::readPage, and no page is ever dirty, so for every
/// trace reads == misses and writes == 0.
///
/// A fresh FileRegistry and BufferPool are built for each call, so the
/// counters start at zero and no other file or engine interferes. The page
/// ids are just numbers in a vector (not pages), so they do not count
/// against B.

#include <cstddef>
#include <filesystem>
#include <vector>

#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/replacement_policy.h"

namespace db::storage
{
    /// Replays `pageIds` (in order) through a new pool of `frames` frames
    /// under `policy` and returns its counters. `dir` is the `--db` folder;
    /// `<dir>/tmp/` is created if needed.
    ///
    /// - An empty list gives a 0-page scratch file and all-zero stats.
    /// - A leftover `trace_scratch.db` from a crashed run is deleted first.
    /// - The scratch file is always deleted before returning, also when an
    ///   exception leaves the function.
    ///
    /// Errors: `frames` == 0 or a page id equal to kInvalidPageId
    /// (0xFFFFFFFF) → std::logic_error (the CLI validates first), checked
    /// before any file is touched. A leftover file that cannot be deleted,
    /// or a disk failure → std::runtime_error.
    IoStats runTrace(const std::filesystem::path& dir, const std::vector<PageId>& pageIds, std::size_t frames,
                     ReplacementPolicy policy);
} // namespace db::storage
