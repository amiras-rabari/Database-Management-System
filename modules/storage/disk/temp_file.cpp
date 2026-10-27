#include "db/storage/temp_file.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace db::storage
{
    namespace
    {
        constexpr std::string_view kPrefix = "temp_";
        constexpr std::string_view kSuffix = ".db";
        // Digits in a name: temp_000001.db. A counter past 999999 just gets
        // a wider name; the names stay unique and in order.
        constexpr std::size_t kNumberWidth = 6;

        // "temp_" + the number padded with zeros to 6 digits + ".db".
        // In: a number such as 7. Out: text such as "temp_000007.db".
        std::string tempName(std::uint64_t number)
        {
            std::string digits = std::to_string(number);
            if (digits.size() < kNumberWidth)
                digits.insert(0, kNumberWidth - digits.size(), '0');
            return std::string(kPrefix) + digits + std::string(kSuffix);
        }

        // True if `name` looks like one of our temp names: "temp_", at least
        // one digit and nothing else, ".db". Checked by hand (no <regex>).
        bool isTempName(const std::string& name)
        {
            if (name.size() <= kPrefix.size() + kSuffix.size())
                return false;
            if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix))
                return false;
            for (std::size_t i = kPrefix.size(); i < name.size() - kSuffix.size(); ++i)
                if (name[i] < '0' || name[i] > '9')
                    return false;
            return true;
        }
    } // namespace

    // --- TempFile -------------------------------------------------------------

    TempFile::TempFile(TempFileManager& manager, FileId fileId)
        : manager_(&manager), fileId_(fileId)
    {
    }

    // Same pattern as PageGuard: copy the fields, empty the source, so only
    // one handle ever deletes the file.
    TempFile::TempFile(TempFile&& other) noexcept
        : manager_(std::exchange(other.manager_, nullptr)),
          fileId_(std::exchange(other.fileId_, 0))
    {
    }

    TempFile& TempFile::operator=(TempFile&& other) noexcept
    {
        if (this != &other)
        {
            // Delete the file we own now. Skipping this would leave it on
            // disk and counted as alive forever. Errors are handled exactly
            // as in the destructor (see below).
            try
            {
                drop();
            }
            catch (const std::runtime_error&)
            {
            }
            manager_ = std::exchange(other.manager_, nullptr);
            fileId_ = std::exchange(other.fileId_, 0);
        }
        return *this;
    }

    TempFile::~TempFile() noexcept
    {
        // A std::logic_error (a page still pinned) is not caught, so it
        // leaves this noexcept destructor and stops the program: a caller bug
        // made loud. A std::runtime_error can only come from the delete on
        // disk, after the file was closed and forgotten; the stale-file sweep
        // of the next TempFileManager removes it, so it is safe to ignore.
        try
        {
            drop();
        }
        catch (const std::runtime_error&)
        {
        }
    }

    void TempFile::requireValid(const char* what) const
    {
        if (manager_ == nullptr)
            throw std::logic_error(std::string("TempFile::") + what + ": the handle owns no file");
    }

    FileId TempFile::fileId() const
    {
        requireValid("fileId");
        return fileId_;
    }

    PageId TempFile::numPages() const
    {
        requireValid("numPages");
        return manager_->pool_.numPages(fileId_);
    }

    bool TempFile::valid() const
    {
        return manager_ != nullptr;
    }

    void TempFile::drop()
    {
        if (manager_ == nullptr)
            return; // already empty, so a second drop does nothing

        // Step 1: tell the buffer pool "forget every page of this file,
        // and don't save them, even the changed ones.
        // If one of the pages is still pinned (someone is still using it),
        // discardFile throws a logic_error before changing anything
        // handle is still valid and the file still exists.
        manager_->pool_.discardFile(fileId_);

        // Make the handle empty BEFORE deleting, so a failed delete can
        // never be retried by this handle (same idea as PageGuard::release).
        TempFileManager* manager = std::exchange(manager_, nullptr);
        const FileId id = std::exchange(fileId_, 0);
        // informs there is one less tempFile that we are managing
        --manager->liveCount_;

        // Step 2: close and delete the file (may throw std::runtime_error).
        manager->files_.remove(id);
    }

    // --- TempFileManager ------------------------------------------------------

    TempFileManager::TempFileManager(std::filesystem::path dbDir, FileRegistry& files, BufferPool& pool)
        : folder_(std::move(dbDir) / "tmp"), files_(files), pool_(pool)
    {
        // Sweep files left by a crashed run. We collect the names first and delete afterwards
        // deleting while a directory_iterator walks the folder is
        // allowed but makes the walk harder to reason about.
        std::error_code error;
        // a missing tmp/ folder isn't an error for us. It just means there's nothing to clean
        if (!std::filesystem::is_directory(folder_, error))
            return;

        std::vector<std::filesystem::path> stale;
        for (const auto& entry : std::filesystem::directory_iterator(folder_))
            if (entry.is_regular_file() && isTempName(entry.path().filename().string()))
                stale.push_back(entry.path());

        for (const auto& path : stale)
        {
            std::filesystem::remove(path, error);
            // takes the error code, so it doesn't throw by itself.
            // we want to show this error occurred so we throw here
            if (error)
                throw std::runtime_error("Cannot delete stale temporary file '" + path.string()
                    + "': " + error.message());
        }
    }

    TempFile TempFileManager::create()
    {
        // Make the tmp/ folder if it doesn't exist yet. If it already exists, this does nothing.
        std::filesystem::create_directories(folder_);
        // Create first, count after: if create throws, the counter and the
        // live count stay as they were.
        const FileId id = files_.create(folder_ / tempName(nextNumber_));
        //  If create had failed, we never reach these lines,
        ++nextNumber_;
        ++liveCount_;
        // remember we pass pointer to manager no refence hence  *this: the object itself not
        // its address "this" is the address, and "*this" dereferences that pointer to get the
        // object stored at that address.
        return TempFile(*this, id);
    }

    std::size_t TempFileManager::liveCount() const
    {
        return liveCount_;
    }

    const std::filesystem::path& TempFileManager::folder() const
    {
        return folder_;
    }
} // namespace db::storage
