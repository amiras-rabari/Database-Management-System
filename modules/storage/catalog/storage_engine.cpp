#include "db/storage/storage_engine.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "db/common/error.h"

namespace db::storage
{
    std::unique_ptr<StorageEngine> StorageEngine::open(const std::filesystem::path& dir, std::size_t frames,
                                                       ReplacementPolicy policy)
    {
        // `new` and not std::make_unique: the constructor is private, and
        // make_unique (not a member) is not allowed to call it.
        return std::unique_ptr<StorageEngine>(new StorageEngine(dir, frames, policy));
    }

    StorageEngine::StorageEngine(std::filesystem::path dir, std::size_t frames, ReplacementPolicy policy)
        : dir_(std::move(dir)), policy_(policy)
    {
        std::filesystem::create_directories(dir_);

        // catalog.db: open it if the folder already has one, else create it
        // empty (0 pages = no tables).
        const std::filesystem::path catalogPath = dir_ / "catalog.db";
        const FileId catalogFile =
            std::filesystem::exists(catalogPath) ? files_.open(catalogPath) : files_.create(catalogPath);

        pool_ = std::make_unique<BufferPool>(frames, policy_, files_);
        catalog_ = std::make_unique<Catalog>(dir_, files_, *pool_, catalogFile);
        // Reads every catalog page once, through the pool (D4).
        catalog_->load();
        // Its constructor sweeps stale temp files.
        temps_ = std::make_unique<TempFileManager>(dir_, files_, *pool_);
    }

    StorageEngine::~StorageEngine() noexcept
    {
        try
        {
            close();
        }
        catch (...)
        {
            // Nothing can be reported from a destructor. Callers that care
            // call close() themselves first.
        }
    }

    Catalog& StorageEngine::catalog()
    {
        return *catalog_;
    }

    const Catalog& StorageEngine::catalog() const
    {
        return *catalog_;
    }

    BufferPool& StorageEngine::pool()
    {
        return *pool_;
    }

    const BufferPool& StorageEngine::pool() const
    {
        return *pool_;
    }

    const std::filesystem::path& StorageEngine::folder() const
    {
        return dir_;
    }

    HeapFile StorageEngine::openHeapFile(const std::string& table)
    {
        std::optional<TableInfo> info = catalog_->getTable(table);
        if (!info)
            throw Error(ErrorCategory::Name, "Unknown table '" + table + "'.");
        // The registry gives back the same id if the file is already open.
        const FileId fileId = files_.open(dir_ / info->dataFile);
        return HeapFile(*pool_, fileId, std::move(info->schema), table);
    }

    TempFile StorageEngine::createTempFile()
    {
        // temps_ is empty only if rebuilding it in resetPool failed.
        if (!temps_)
            throw std::logic_error("StorageEngine::createTempFile: no temp-file manager (resetPool failed)");
        return temps_->create();
    }

    std::size_t StorageEngine::liveTempFileCount() const
    {
        return temps_ ? temps_->liveCount() : 0;
    }

    void StorageEngine::flushAll()
    {
        // The catalog first: save() leaves its pages dirty in the pool, and
        // the pool's flushAll then writes them with everything else.
        catalog_->save();
        pool_->flushAll();
    }

    void StorageEngine::requireIdle(const char* what) const
    {
        if (pool_->pinnedFrameCount() > 0)
            throw std::logic_error(std::string("StorageEngine::") + what + ": "
                + std::to_string(pool_->pinnedFrameCount()) + " page(s) still pinned");
        if (liveTempFileCount() > 0)
            throw std::logic_error(std::string("StorageEngine::") + what + ": "
                + std::to_string(liveTempFileCount()) + " temp file(s) still alive");
    }

    void StorageEngine::close()
    {
        if (closed_)
            return;
        requireIdle("close");
        flushAll();
        closed_ = true;
    }

    void StorageEngine::resetPool(std::size_t frames)
    {
        requireIdle("resetPool");
        flushAll();

        // Build the new pool first: if `frames` is bad its constructor
        // throws here and the engine is unchanged.
        auto newPool = std::make_unique<BufferPool>(frames, policy_, files_);

        // The catalog and the temp manager keep a reference to the pool. The
        // catalog is repointed (its map must stay). The temp manager is
        // rebuilt: it has no live files (checked above), so it holds nothing
        // worth keeping, and TempFileManager stores a plain `BufferPool&`,
        // which cannot be repointed. It is destroyed BEFORE the old pool, so
        // it never refers to a dead pool.
        temps_.reset();
        pool_ = std::move(newPool); // destroys the old pool (all its pages are clean)
        catalog_->usePool(*pool_);
        // If its stale-file sweep throws, temps_ stays empty: the pool and
        // catalog still work, createTempFile then refuses.
        temps_ = std::make_unique<TempFileManager>(dir_, files_, *pool_);
    }
} // namespace db::storage
