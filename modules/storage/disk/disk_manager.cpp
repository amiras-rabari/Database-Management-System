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

        // std::uintmax_t The largest unsigned integer type the compiler has.
        // No header in the file (§M1.2): the page count is the size / 4096.
        const std::uintmax_t size = fs::file_size(path);
        if (size % kPageSize != 0)
            throw std::runtime_error("File " + quoted(path) + " is corrupt: its size " + std::to_string(size)
                                     + " is not a multiple of " + std::to_string(kPageSize) + " bytes.");
        const std::uintmax_t pages = size / kPageSize;
        if (pages > kInvalidPageId)
            throw std::runtime_error("File " + quoted(path) + " has more pages than a page id can address.");

        // Opened once now to report an unreadable file at once, then closed:
        // the first page call opens it again (see the class comment).
        DiskManager manager(path, static_cast<PageId>(pages));
        manager.stream();
        manager.closeFile();
        return manager;
    }

    DiskManager DiskManager::create(const fs::path& path)
    {
        if (fs::exists(path))
            throw Error(ErrorCategory::Name, "File " + quoted(path) + " already exists.");
        // trunc is what makes an in|out stream create the file; it is
        // harmless here because the file did not exist a moment ago so nothing to loose in terms of data.
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            throw std::runtime_error("Cannot create file " + quoted(path) + ".");
        // `file` closes here; the first page call opens it again.
        return DiskManager(path, 0);
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

        std::fstream& file = stream();
        file.seekg(byteOffset(pageId));
        // char may alias any object, so viewing the std::byte buffer as char
        // for the stream is allowed (the reverse cast would not be).
        // reinterpret_cast<char*>(p) "Look at the same memory as a different type"

        // int can be used here as well for streamsize but explicitly casting gives much mor readability
        // and type safety for as it intel this is IO related type telling how many bytes to read from file
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(kPageSize));
        if (!file || file.gcount() != static_cast<std::streamsize>(kPageSize))
        {
            file.clear();
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

        // seekp(): Stands for "seek put". It moves the file's internal write pointer (the "cursor")
        // to a specific position.
        std::fstream& file = stream();
        file.seekp(byteOffset(pageId));

        // bytes.data() grabs the raw memory pointer to the underlying buffer
        // and reinterpret_cast<const char*> says treat this raw binary memory buffer as a stream of
        // generic characters so we can write bytes safely
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(kPageSize));

        // Flushed at once, so the file on disk always matches the counted calls.
        // flush ensures that Stop waiting and force all buffered data out of memory and
        // write it onto the physical disk right now
        file.flush();

        //In C++, checking a file stream like an if statement (if (!file)) checks the stream's error flags.
        //If the stream is in a "bad state"—meaning the write or the flush failed then it becomes true
        if (!file)
        {
            file.clear();
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
        std::fstream& file = stream();
        file.seekp(byteOffset(numPages_));
        for (std::size_t written = 0; written < kPageSize; written += zeros.size())
            file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        file.flush();
        if (!file)
        {
            file.clear();
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

    DiskManager::DiskManager(fs::path path, PageId numPages) : path_(std::move(path)), numPages_(numPages)
    {
    }

    std::fstream& DiskManager::stream()
    {
        if (!file_)
        {
            // std::fstream: a file that can be both read and written. It
            // closes itself when it is destroyed (closeFile, or with us).
            std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
            if (!file.is_open())
                throw std::runtime_error("Cannot open file " + quoted(path_) + ".");
            file_ = std::move(file);
        }
        return *file_;
    }

    void DiskManager::closeFile()
    {
        file_.reset();
    }

    bool DiskManager::isOpen() const
    {
        return file_.has_value();
    }

} // namespace db::storage
