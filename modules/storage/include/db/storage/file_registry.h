#pragma once

/// @file
/// The list of every open page file (tables, the catalog, temporary files)
/// and the number (FileId) that names each one.
///
/// Page numbers start again at 0 in every file (§M1.2), so "page 3" alone
/// is not enough: page 3 of R.db and page 3 of S.db are different pages.
/// That is why the one shared buffer pool names a page with two numbers:
/// (FileId, PageId).

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>

#include "db/storage/disk_manager.h"

namespace db::storage
{
    /// The number that names one open file. Handed out by a counter in the
    /// order files are opened (0, 1, 2, ...) and never reused after a file is
    /// removed, so the same steps always give the same ids.
    using FileId = std::uint32_t;

    /// Owns the DiskManager of every open file. Everyone else only borrows a
    /// reference, which stays valid while the file is registered.
    /// It cannot be copied or moved, because the buffer pool keeps a
    /// reference to it.
    ///
    /// Errors: using a FileId that is not registered is a bug in the caller
    /// (std::logic_error, D1). Problems with the file itself come from
    /// DiskManager: db::Error(Name) for a missing or already existing file,
    /// std::runtime_error for read/write failures.
    class FileRegistry
    {
    public:
        FileRegistry() = default;
        FileRegistry(const FileRegistry&) = delete;
        FileRegistry& operator=(const FileRegistry&) = delete;
        FileRegistry(FileRegistry&&) = delete;
        FileRegistry& operator=(FileRegistry&&) = delete;

        /// Opens a file that already exists and returns its new FileId.
        /// If this path is already open, it returns the id it already has,
        /// so one file never gets two disk managers. Throws like DiskManager::open.
        FileId open(const std::filesystem::path& path);

        /// Creates a new, empty file (0 pages) and returns its new FileId.
        /// Throws db::Error(Name) if the file already exists, whether or not
        /// it is open. An existing file is never emptied by accident.
        FileId create(const std::filesystem::path& path);

        /// True if `id` names an open file.
        bool contains(FileId id) const;

        /// The disk manager for file `id`. Throws std::logic_error if `id`
        /// is not registered.
        /// Returns a mutable reference, allowing calling code to modify the underlying DiskManager
        DiskManager& disk(FileId id);

        ///Const Overload: Executed when called on a const FileRegistry object. Marked const at the
        ///end (meaning it won't modify FileRegistry state) and returns a read-only (const) reference.
        const DiskManager& disk(FileId id) const;

        /// Closes file `id` and deletes it from the folder (used for
        /// temporary files, chunk 10). First drop its pages from the buffer
        /// pool with BufferPool::discardFile: the registry does not know the
        /// pool exists. Throws std::logic_error if `id` is not registered,
        /// std::runtime_error if the file cannot be deleted.
        void remove(FileId id);

    private:
        /// Owns every disk manager, keyed by FileId. A std::map (not an
        /// unordered_map), so looping over it always goes in id order.
        std::map<FileId, std::unique_ptr<DiskManager>> disks_;
        /// path -> FileId of every open file, to spot "this path is already
        /// open". The keys are full, tidied-up paths (see pathKey).
        std::map<std::filesystem::path, FileId> idsByPath_;
        /// The id the next open or create will hand out. same as std::uint32_t nextId = 0;
        FileId nextId_ = 0;
    };
} // namespace db::storage
