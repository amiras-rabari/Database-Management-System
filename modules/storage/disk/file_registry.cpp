#include "db/storage/file_registry.h"

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace db::storage
{
    namespace
    {
        // Turns a path into the key used in idsByPath_:
        // .lexically_normal(): Performs pure string-based cleanup to remove redundant
        // elements like . (current directory), .. (parent directory), and extra slashes (//)
        // Works without touching the disk, so it won't crash on new/uncreated files.
        // Uses lexically_normal() instead of canonical() so it won't crash on files
        // that don't exist yet, avoiding slow disk checks and unwanted symlink resolution.
        std::filesystem::path pathKey(const std::filesystem::path& path)
        {
            return std::filesystem::absolute(path).lexically_normal();
        }

        // The error for a FileId that is not registered (a caller bug, D1).
        std::logic_error unknownFile(const char* where, FileId id)
        {
            return std::logic_error(std::string(where) + ": file " + std::to_string(id)
                + " is not registered");
        }
    } // namespace

    FileId FileRegistry::open(const std::filesystem::path& path)
    {
        const std::filesystem::path key = pathKey(path);
        // Already open: give back its id, so one file never gets a second
        // disk manager.
        // if with initializer same as creating found first and then running condition
        //found->first The Key (e.g., std::filesystem::path)
        //found->second The Value (e.g., FileId / int)
        // .find(key) does one lookup by hash and  returns an iterator to the element if found,
        // or end() if not found.
        if (const auto found = idsByPath_.find(key); found != idsByPath_.end())
            return found->second;

        // Open the file first. If this throws, nothing has been added yet.
        DiskManager disk = DiskManager::open(path);
        const FileId id = nextId_++;
        disks_[id] = std::make_unique<DiskManager>(std::move(disk));
        idsByPath_[key] = id;
        return id;
    }

    FileId FileRegistry::create(const std::filesystem::path& path)
    {
        // No "already open" check here: an open file exists on disk, so
        // DiskManager::create already throws db::Error(Name) for it.
        DiskManager disk = DiskManager::create(path);
        const FileId id = nextId_++;
        disks_[id] = std::make_unique<DiskManager>(std::move(disk));
        idsByPath_[pathKey(path)] = id;
        return id;
    }

    bool FileRegistry::contains(FileId id) const
    {
        // .find(key) does one lookup by hash and  returns an iterator to the element if found,
        // or end() if not found.
        return disks_.find(id) != disks_.end();
    }

    DiskManager& FileRegistry::disk(FileId id)
    {
        const auto found = disks_.find(id);
        if (found == disks_.end())
            throw unknownFile("FileRegistry::disk", id);
        //  found->second The Value (e.g., DiskManager)
        return *found->second;
    }

    const DiskManager& FileRegistry::disk(FileId id) const
    {
        const auto found = disks_.find(id);
        if (found == disks_.end())
            throw unknownFile("FileRegistry::disk", id);
        return *found->second;
    }

    void FileRegistry::remove(FileId id)
    {
        const auto found = disks_.find(id);
        if (found == disks_.end())
            throw unknownFile("FileRegistry::remove", id);

        // Copy the path now: it is stored inside the disk manager, which is
        // destroyed two lines below.
        const std::filesystem::path path = found->second->path();
        idsByPath_.erase(pathKey(path));
        // Destroying the DiskManager closes the file. It must be closed
        // before deleting, because Windows cannot delete an open file.
        // nextId_ does not go back down: ids are never reused.
        disks_.erase(found);

        std::error_code error;
        std::filesystem::remove(path, error);
        if (error)
            throw std::runtime_error("Cannot delete file '" + path.string() + "': " + error.message());
    }
} // namespace db::storage
