#include "db/storage/trace.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>

#include "db/storage/buffer_pool.h"
#include "db/storage/disk_manager.h"
#include "db/storage/file_registry.h"
#include "db/storage/page_guard.h"

namespace db::storage
{
    namespace
    {
        // Owns the scratch file while the trace runs. Like TempFile, but
        // private to this file and with a fixed name. remove() drops the
        // file's pages from the pool (no writes) and deletes the file; the
        // destructor does the same if remove() was never reached, i.e. when
        // an exception is leaving runTrace.
        class ScratchFile
        {
        public:
            ScratchFile(FileRegistry& files, BufferPool& pool, FileId id) : files_(files), pool_(pool), id_(id) {}

            ScratchFile(const ScratchFile&) = delete;
            ScratchFile& operator=(const ScratchFile&) = delete;

            // Normal path: errors (a failed delete) reach the caller.
            void remove()
            {
                if (removed_)
                    return;
                removed_ = true; // first, so a failed delete is not tried twice
                pool_.discardFile(id_);
                files_.remove(id_);
            }

            // Exception path: an exception is already on its way out, and a
            // destructor must not throw a second one. A file left behind is
            // deleted by the next runTrace before it starts.
            ~ScratchFile()
            {
                try
                {
                    remove();
                }
                catch (const std::exception&)
                {
                }
            }

        private:
            FileRegistry& files_;
            BufferPool& pool_;
            FileId id_;
            bool removed_ = false;
        };
    } // namespace

    IoStats runTrace(const std::filesystem::path& dir, const std::vector<PageId>& pageIds, std::size_t frames,
                     ReplacementPolicy policy)
    {
        // 1. Check the arguments before touching any file.
        if (frames == 0)
            throw std::logic_error("runTrace: frames must be at least 1");
        if (std::find(pageIds.begin(), pageIds.end(), kInvalidPageId) != pageIds.end())
            throw std::logic_error("runTrace: page id 4294967295 (0xFFFFFFFF) is not a page");

        // 2. The scratch file's name is fixed (deterministic). A file with
        //    that name can only be left over from a run that crashed.
        const std::filesystem::path folder = dir / "tmp";
        const std::filesystem::path path = folder / "trace_scratch.db";
        std::filesystem::create_directories(folder);
        std::error_code error;
        std::filesystem::remove(path, error);
        if (error)
            throw std::runtime_error("Cannot delete old trace file '" + path.string() + "': " + error.message());

        // 3. A fresh registry and pool: counters start at zero. Declared in
        //    this order so the pool (which refers to the registry) and the
        //    scratch file (which refers to both) are destroyed first.
        FileRegistry files;
        BufferPool pool(frames, policy, files);
        const FileId fileId = files.create(path);
        ScratchFile scratch(files, pool, fileId);

        // 4. Pages 0 .. max id: allocatePage writes zeros straight to the
        //    file and counts nothing (D2), so the stats stay at zero.
        //    An empty trace needs no pages.
        if (!pageIds.empty())
        {
            const PageId maxId = *std::max_element(pageIds.begin(), pageIds.end());
            DiskManager& disk = files.disk(fileId);
            for (PageId p = 0; p <= maxId; ++p)
                disk.allocatePage();
        }

        // 5. The replay (§M1.9): fetch each id in order (the pool counts the
        //    hit or miss), then unpin it at once, clean (no markDirty).
        for (const PageId pageId : pageIds)
        {
            PageGuard guard = PageGuard::fetch(pool, fileId, pageId);
            guard.release();
        }

        // 6. Snapshot first, then delete the file (removing writes nothing,
        //    but the counters are taken before any cleanup to be safe).
        const IoStats stats = pool.stats();
        scratch.remove();
        return stats;
    }
} // namespace db::storage
