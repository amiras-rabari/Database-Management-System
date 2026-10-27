#pragma once

/// @file
/// Temporary files for exec (sort runs, hash partitions), chunk 10.
///
/// Only storage may touch files (S1), so exec cannot create or delete a file
/// itself. Instead it asks for a TempFile: a handle that owns one new, empty
/// page file inside `<DIR>/tmp/`. Exec reads and writes its pages through the
/// one shared buffer pool (fetchPage / newPage / PageGuard with fileId()),
/// so temp pages use the same B frames and are counted in IoStats like any
/// other page (S2). When the handle goes away, the file is deleted.
///
/// The page format of a run (RunWriter / RunReader) is exec's job, not this
/// file's. Exec gets TempFiles from StorageEngine::createTempFile (chunk 12),
/// which forwards to the TempFileManager below.

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/page.h"

namespace db::storage
{
    class TempFileManager;

    /// Owns one temporary file. Either *valid* (it owns a file) or *empty*
    /// (after drop(), after being moved from, or made with TempFile()).
    ///
    /// Move-only, for the same reason as PageGuard: two copies would both
    /// delete the one file.
    ///
    /// Rules for the caller:
    /// - Release every PageGuard on this file before the TempFile dies.
    ///   drop() refuses to run while a page of the file is pinned.
    /// - A TempFile must not outlive its TempFileManager or the buffer pool
    ///   (chunk 12's close()/resetPool() refuse while one is alive).
    class TempFile
    {
    public:
        /// An empty handle (assign one later).
        TempFile() = default;

        /// Rule of five, as in PageGuard: no copies, moves hand the file over.
        TempFile(const TempFile&) = delete;
        TempFile& operator=(const TempFile&) = delete;
        /// The new handle takes over `other`'s file; `other` becomes empty.
        TempFile(TempFile&& other) noexcept;
        /// First drops this handle's own file (if any), then takes over
        /// `other`'s. Assigning a handle to itself does nothing.
        TempFile& operator=(TempFile&& other) noexcept;
        /// Drops the file if the handle is still valid. noexcept: if the drop
        /// fails because a page is still pinned (a caller bug), the program
        /// stops, which makes the bug obvious. If only the delete on disk
        /// fails, the file is already closed and forgotten; the leftover file
        /// is removed the next time a TempFileManager opens the folder.
        ~TempFile() noexcept;

        /// The file's id, for fetchPage / newPage / PageGuard.
        /// Empty handle → std::logic_error.
        FileId fileId() const;

        /// How many pages the file has now (no I/O). Pages are numbered
        /// 0 … numPages() − 1, in the order newPage added them.
        /// Empty handle → std::logic_error.
        PageId numPages() const;

        /// True while the handle owns a file.
        bool valid() const;

        /// Deletes the file now and makes the handle empty:
        /// 1. throws the file's pages out of the pool **without writing
        ///    them**, even if they are dirty (nobody will read them again, so
        ///    writing them would only add writes to the counters);
        /// 2. closes and deletes the file.
        /// A page of the file still pinned → std::logic_error, and nothing
        /// changes (the handle stays valid, the file still exists).
        /// The file cannot be deleted on disk → std::runtime_error (the
        /// handle is empty by then). On an empty handle it does nothing.
        void drop();

    private:
        // grants access of TempFile private and public methods to TempFileManager
        friend class TempFileManager;
        TempFile(TempFileManager& manager, FileId fileId);

        /// Throws std::logic_error, naming the function `what`, if empty.
        void requireValid(const char* what) const;

        /// The manager that made the file, or nullptr for an empty handle.
        /// A pointer, not a reference, so move assignment can repoint it.
        /// Storing a C++ pointer field gives a flexible, re-aimable spotlight—you can create
        /// an object anywhere outside, shine the pointer at it, and later re-aim that spotlight
        /// at a completely different object whenever  need mutability or reassignment
        TempFileManager* manager_ = nullptr;
        FileId fileId_ = 0;
    };

    /// Makes TempFiles inside `<DIR>/tmp/` and counts how many are alive.
    /// One per engine (chunk 12). Not copyable or movable, because every
    /// TempFile points back to it.
    class TempFileManager
    {
    public:
        /// `dbDir` is the `--db DIR` folder. `files` and `pool` must outlive
        /// the manager.
        ///
        /// Stale files: if `<dbDir>/tmp/` exists, every file in it whose name
        /// has our pattern (`temp_` + digits + `.db`) is deleted now. Such a
        /// file can only be left over from a run that crashed, so every open
        /// starts from the same empty folder (determinism). Other files, and
        /// everything outside `tmp/`, are never touched.
        /// A stale file that cannot be deleted → std::runtime_error.
        TempFileManager(std::filesystem::path dbDir, FileRegistry& files, BufferPool& pool);

        TempFileManager(const TempFileManager&) = delete;
        TempFileManager& operator=(const TempFileManager&) = delete;
        TempFileManager(TempFileManager&&) = delete;
        TempFileManager& operator=(TempFileManager&&) = delete;

        /// Creates the next empty file `tmp/temp_000001.db`, `temp_000002.db`,
        /// … (a counter starting at 1 for each manager: never the time, a
        /// process id or a random number, so the same commands always give
        /// the same names). Creates `tmp/` on first use. Throws like
        /// FileRegistry::create; on a throw the counter does not move on and
        /// nothing is counted as alive.
        TempFile create();

        /// How many TempFiles made here are still alive (not dropped).
        std::size_t liveCount() const;

        /// The folder the temp files go in: `<dbDir>/tmp`.
        const std::filesystem::path& folder() const;

    private:
        friend class TempFile;

        std::filesystem::path folder_;
        FileRegistry& files_;
        BufferPool& pool_;
        /// The number in the next file's name.
        std::uint64_t nextNumber_ = 1;
        std::size_t liveCount_ = 0;
    };
} // namespace db::storage
