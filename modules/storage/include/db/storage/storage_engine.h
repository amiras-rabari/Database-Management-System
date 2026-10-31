#pragma once

/// @file
/// The storage engine (chunk 12): the one object every other module opens.
/// Given the `--db DIR` folder, the number of frames B and a replacement
/// policy, it owns the whole storage stack (file registry, the one buffer
/// pool, the catalog, the temp-file manager) and hands out heap files and
/// temp files.
///
/// Lifetime rules for callers:
/// - HeapFile, HeapScan, PageGuard and TempFile refer to the engine's pool.
///   None of them may outlive the engine, nor survive a resetPool().
/// - close() and resetPool() refuse to run while a page is pinned or a
///   TempFile is alive.
/// - Counters: the catalog pages read by open() are counted in the pool's
///   stats. Take a command's snapshot (`pool().stats()`) after open and
///   after the getTable lookups (D4); catalog lookups themselves cost no I/O.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "db/storage/buffer_pool.h"
#include "db/storage/catalog.h"
#include "db/storage/file_registry.h"
#include "db/storage/heap_file.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/temp_file.h"

namespace db::storage
{
    /// Neither copyable nor movable: its parts keep references to each
    /// other (the pool to the registry, the catalog and temp manager to
    /// both), so the engine must stay at one address. That is why open()
    /// returns a std::unique_ptr.
    class StorageEngine
    {
    public:
        /// Opens the database in `dir`: creates the folder if needed, opens
        /// `catalog.db` (or creates it, empty, in a new folder), builds a pool
        /// of `frames` frames (≥ 1, else std::logic_error) with `policy`,
        /// reads the catalog through it, and deletes stale temp files in
        /// `<dir>/tmp/` (chunk 10). A corrupt catalog → std::runtime_error.
        static std::unique_ptr<StorageEngine> open(const std::filesystem::path& dir, std::size_t frames,
                                                   ReplacementPolicy policy);

        StorageEngine(const StorageEngine&) = delete;
        StorageEngine& operator=(const StorageEngine&) = delete;
        StorageEngine(StorageEngine&&) = delete;
        StorageEngine& operator=(StorageEngine&&) = delete;

        /// Calls close(), ignoring any error (a destructor must not throw).
        /// Call close() yourself to see errors.
        ~StorageEngine() noexcept;

        Catalog& catalog();
        const Catalog& catalog() const;
        /// The current pool. After resetPool() this is a different object:
        /// do not keep the reference across a resetPool.
        BufferPool& pool();
        const BufferPool& pool() const;

        /// The database folder.
        const std::filesystem::path& folder() const;

        /// A handle on table `table`'s heap file, with the schema qualified by
        /// the table name. Opens `<table>.db` the first time (later calls get
        /// the same FileId). No page is read. Unknown table →
        /// db::Error(Name) "Unknown table 'X'.".
        HeapFile openHeapFile(const std::string& table);

        /// A new empty temp file (TempFileManager::create).
        TempFile createTempFile();
        /// How many temp files are alive.
        std::size_t liveTempFileCount() const;

        /// Saves the catalog if it changed, then writes every dirty page
        /// (BufferPool::flushAll). Counts the writes in the pool's stats.
        void flushAll();

        /// Refuses (std::logic_error, nothing done) while a page is pinned or
        /// a TempFile is alive; otherwise flushAll(). Calling it again does
        /// nothing. After close() the engine should only be destroyed.
        void close();

        /// For `SET frames = n`: refuses like close(); then flushAll() and
        /// replaces the pool with a new one of `frames` frames (≥ 1, else
        /// std::logic_error, nothing changed) and the same policy. The new
        /// pool's counters start at zero and it holds no pages. The temp-file
        /// manager is rebuilt on the new pool (its file numbers restart at 1).
        void resetPool(std::size_t frames);

    private:
        StorageEngine(std::filesystem::path dir, std::size_t frames, ReplacementPolicy policy);

        /// Throws std::logic_error naming `what` if a page is pinned or a
        /// temp file is alive.
        void requireIdle(const char* what) const;

        std::filesystem::path dir_;
        ReplacementPolicy policy_;
        bool closed_ = false;

        // Declared in dependency order: each part uses only the parts above
        // it. C++ builds members top to bottom and destroys them bottom to
        // top, so the temp manager goes first, then the catalog, then the
        // pool, and the registry (whose files they all use) goes last.
        FileRegistry files_;
        /// A unique_ptr so resetPool can swap in a new pool.
        std::unique_ptr<BufferPool> pool_;
        std::unique_ptr<Catalog> catalog_;
        /// A unique_ptr so resetPool can rebuild it on the new pool.
        std::unique_ptr<TempFileManager> temps_;
    };
} // namespace db::storage
