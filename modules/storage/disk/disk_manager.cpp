#include "db/storage/disk_manager.h"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

#include "db/common/error.h"

namespace db::storage
{

    namespace fs = std::filesystem;

    namespace
    {

        // Writes come from this small block of zeros, repeated, so that
        // allocatePage never holds a whole page in memory (the B-page budget
        // belongs to the buffer pool).
        constexpr std::size_t kZeroBlockSize = 256;
        static_assert(kPageSize % kZeroBlockSize == 0);

        // The file name as it appears in error messages: 'path'.
        std::string quoted(const fs::path& path)
        {
            return "'" + path.string() + "'";
        }

        // Byte position of a page in its file, computed in 64 bits: in 32-bit
        // arithmetic pageId * 4096 overflows from page id 1048576 on.
        std::streamoff byteOffset(PageId pageId)
        {
            return static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(kPageSize);
        }

        // Throws std::logic_error unless the caller's buffer is exactly one page.
        void checkBufferSize(std::size_t size, const char* function)
        {
            if (size != kPageSize)
                throw std::logic_error(std::string(function) + ": buffer of " + std::to_string(size)
                                       + " bytes, expected " + std::to_string(kPageSize));
        }

    } // namespace

    DiskManager DiskManager::open(const fs::path& path)
    {
        if (!fs::exists(path))
            throw Error(ErrorCategory::Name, "File " + quoted(path) + " does not exist.");
        if (!fs::is_regular_file(path))
            throw std::runtime_error("File " + quoted(path) + " is not a regular file.");

        // No header in the file (§M1.2): the page count is the size / 4096.
        const std::uintmax_t size = fs::file_size(path);
        if (size % kPageSize != 0)
            throw std::runtime_error("File " + quoted(path) + " is corrupt: its size " + std::to_string(size)
                                     + " is not a multiple of " + std::to_string(kPageSize) + " bytes.");
        const std::uintmax_t pages = size / kPageSize;
        if (pages > kInvalidPageId)
            throw std::runtime_error("File " + quoted(path) + " has more pages than a page id can address.");

        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file.is_open())
            throw std::runtime_error("Cannot open file " + quoted(path) + ".");
        return DiskManager(path, std::move(file), static_cast<PageId>(pages));
    }

    DiskManager DiskManager::create(const fs::path& path)
    {
        if (fs::exists(path))
            throw Error(ErrorCategory::Name, "File " + quoted(path) + " already exists.");
        // trunc is what makes an in|out stream create the file; it is
        // harmless here because the file did not exist a moment ago.
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            throw std::runtime_error("Cannot create file " + quoted(path) + ".");
        return DiskManager(path, std::move(file), 0);
    }

    const fs::path& DiskManager::path() const
    {
        return path_;
    }

    PageId DiskManager::numPages() const
    {
        return numPages_;
    }

    void DiskManager::readPage(PageId pageId, std::span<std::byte> out)
    {
        checkBufferSize(out.size(), "readPage");
        if (pageId >= numPages_)
            throw std::logic_error("readPage: page " + std::to_string(pageId) + " is not in file " + quoted(path_)
                                   + " (" + std::to_string(numPages_) + " pages)");

        file_.seekg(byteOffset(pageId));
        // char may alias any object, so viewing the std::byte buffer as char
        // for the stream is allowed (the reverse cast would not be).
        file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(kPageSize));
        if (!file_ || file_.gcount() != static_cast<std::streamsize>(kPageSize))
        {
            file_.clear();
            throw std::runtime_error("Short read of page " + std::to_string(pageId) + " from file " + quoted(path_)
                                     + ".");
        }
        ++reads_;
    }

    void DiskManager::writePage(PageId pageId, std::span<const std::byte> bytes)
    {
        checkBufferSize(bytes.size(), "writePage");
        if (pageId >= numPages_)
            throw std::logic_error("writePage: page " + std::to_string(pageId) + " was never allocated in file "
                                   + quoted(path_) + " (" + std::to_string(numPages_) + " pages)");

        file_.seekp(byteOffset(pageId));
        file_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(kPageSize));
        // Flushed at once, so the file on disk always matches the counted calls.
        file_.flush();
        if (!file_)
        {
            file_.clear();
            throw std::runtime_error("Failed to write page " + std::to_string(pageId) + " of file " + quoted(path_)
                                     + ".");
        }
        ++writes_;
    }

    PageId DiskManager::allocatePage()
    {
        if (numPages_ == kInvalidPageId)
            throw std::runtime_error("File " + quoted(path_) + " is full: no page id left to allocate.");

        static constexpr std::array<char, kZeroBlockSize> zeros{};
        file_.seekp(byteOffset(numPages_));
        for (std::size_t written = 0; written < kPageSize; written += zeros.size())
            file_.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        file_.flush();
        if (!file_)
        {
            file_.clear();
            throw std::runtime_error("Failed to allocate page " + std::to_string(numPages_) + " in file "
                                     + quoted(path_) + ".");
        }
        // Not counted as a write (M1 decision D2): it is not a writePage call.
        return numPages_++;
    }

    std::uint64_t DiskManager::readCount() const
    {
        return reads_;
    }

    std::uint64_t DiskManager::writeCount() const
    {
        return writes_;
    }

    DiskManager::DiskManager(fs::path path, std::fstream file, PageId numPages)
        : path_(std::move(path)), file_(std::move(file)), numPages_(numPages)
    {
    }

} // namespace db::storage
