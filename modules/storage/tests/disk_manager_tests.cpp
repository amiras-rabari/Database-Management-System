// DiskManager: page count from file size, read/write counters, reopen,
// out-of-range errors and refusal of corrupt files (SPEC §M1.2, §M1.6).

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/storage/disk_manager.h"
#include "db/storage/page.h"

namespace
{

    namespace fs = std::filesystem;
    using db::ErrorCategory;
    using db::storage::DiskManager;
    using db::storage::kPageSize;
    using Page = std::array<std::byte, kPageSize>;

    // A fresh folder under the system temp directory, removed when the test
    // starts and again when it ends. Each test case uses its own name.
    class TempDir
    {
    public:
        explicit TempDir(const std::string& name)
            : path_(fs::temp_directory_path() / "comp3005_storage_tests" / name)
        {
            fs::remove_all(path_);
            fs::create_directories(path_);
        }
        ~TempDir()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
        const fs::path& path() const { return path_; }

    private:
        fs::path path_;
    };

    // A page whose bytes depend on their position, so a read of the wrong
    // page or a shifted read cannot match it.
    Page pattern()
    {
        Page page{};
        for (std::size_t i = 0; i < page.size(); ++i)
            page[i] = static_cast<std::byte>((i * 7 + 3) % 251);
        return page;
    }

    // Runs `function` and returns the db::Error it threw, or nullopt.
    template <typename Function>
    std::optional<db::Error> errorFrom(Function function)
    {
        try
        {
            function();
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    // Runs `function` and returns the what() of the std::runtime_error it
    // threw (not a db::Error), or nullopt.
    template <typename Function>
    std::optional<std::string> runtimeErrorFrom(Function function)
    {
        try
        {
            function();
        }
        catch (const db::Error&)
        {
            return std::nullopt;
        }
        catch (const std::runtime_error& error)
        {
            return std::string(error.what());
        }
        return std::nullopt;
    }

} // namespace

TEST_CASE("a new file has no pages and size 0")
{
    TempDir dir("new_file");
    DiskManager disk = DiskManager::create(dir.path() / "t.db");
    CHECK(disk.numPages() == 0);
    CHECK(fs::file_size(dir.path() / "t.db") == 0);
    CHECK(disk.readCount() == 0);
    CHECK(disk.writeCount() == 0);
}

TEST_CASE("allocatePage appends zero pages and counts no write")
{
    TempDir dir("allocate");
    DiskManager disk = DiskManager::create(dir.path() / "t.db");
    CHECK(disk.allocatePage() == 0);
    CHECK(disk.allocatePage() == 1);
    CHECK(disk.allocatePage() == 2);
    CHECK(disk.numPages() == 3);
    CHECK(fs::file_size(dir.path() / "t.db") == 3 * kPageSize);
    CHECK(disk.writeCount() == 0);
    CHECK(disk.readCount() == 0);

    // A freshly allocated page reads back as zeros.
    Page page = pattern();
    disk.readPage(2, page);
    CHECK(page == Page{});
    CHECK(disk.readCount() == 1);
}

TEST_CASE("write then read a page: identical bytes, one read and one write")
{
    TempDir dir("write_read");
    DiskManager disk = DiskManager::create(dir.path() / "t.db");
    for (int i = 0; i < 3; ++i)
        disk.allocatePage();

    const Page written = pattern();
    disk.writePage(1, written);
    Page read{};
    disk.readPage(1, read);
    CHECK(read == written);
    CHECK(disk.readCount() == 1);
    CHECK(disk.writeCount() == 1);

    // Neighbouring pages are untouched.
    disk.readPage(0, read);
    CHECK(read == Page{});
    disk.readPage(2, read);
    CHECK(read == Page{});
    CHECK(disk.readCount() == 3);
}

TEST_CASE("reopening a file keeps its pages and starts counters at 0")
{
    TempDir dir("reopen");
    const fs::path file = dir.path() / "t.db";
    {
        DiskManager disk = DiskManager::create(file);
        for (int i = 0; i < 3; ++i)
            disk.allocatePage();
        disk.writePage(1, pattern());
    }
    DiskManager disk = DiskManager::open(file);
    CHECK(disk.numPages() == 3);
    CHECK(disk.readCount() == 0);
    CHECK(disk.writeCount() == 0);
    Page read{};
    disk.readPage(1, read);
    CHECK(read == pattern());
}

TEST_CASE("reading or writing a page that was never allocated is a logic_error")
{
    TempDir dir("out_of_range");
    DiskManager disk = DiskManager::create(dir.path() / "t.db");
    for (int i = 0; i < 3; ++i)
        disk.allocatePage();

    Page page{};
    CHECK_THROWS_AS(disk.readPage(3, page), std::logic_error);
    CHECK_THROWS_AS(disk.writePage(5, page), std::logic_error);
    CHECK_THROWS_AS(disk.writePage(3, page), std::logic_error);
    // Failed calls are not counted.
    CHECK(disk.readCount() == 0);
    CHECK(disk.writeCount() == 0);
    // The file did not grow.
    CHECK(disk.numPages() == 3);
    CHECK(fs::file_size(dir.path() / "t.db") == 3 * kPageSize);
}

TEST_CASE("a buffer that is not exactly one page is a logic_error")
{
    TempDir dir("wrong_buffer");
    DiskManager disk = DiskManager::create(dir.path() / "t.db");
    disk.allocatePage();

    std::array<std::byte, kPageSize - 1> small{};
    std::array<std::byte, kPageSize + 1> large{};
    CHECK_THROWS_AS(disk.readPage(0, small), std::logic_error);
    CHECK_THROWS_AS(disk.writePage(0, large), std::logic_error);
    CHECK(disk.readCount() == 0);
    CHECK(disk.writeCount() == 0);
}

TEST_CASE("open of a missing file and create of an existing file are name errors")
{
    TempDir dir("open_create_errors");
    const fs::path missing = dir.path() / "missing.db";
    auto error = errorFrom([&] { DiskManager::open(missing); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message().find("missing.db") != std::string::npos);
    CHECK_FALSE(fs::exists(missing));

    const fs::path existing = dir.path() / "existing.db";
    {
        DiskManager disk = DiskManager::create(existing);
        disk.allocatePage();
    }
    error = errorFrom([&] { DiskManager::create(existing); });
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Name);
    CHECK(error->message().find("existing.db") != std::string::npos);
    // create never truncates.
    CHECK(fs::file_size(existing) == kPageSize);
}

TEST_CASE("a file whose size is not a multiple of 4096 is refused, naming the file")
{
    TempDir dir("corrupt");
    const fs::path file = dir.path() / "bad.db";
    {
        std::ofstream out(file, std::ios::binary);
        const std::string bytes(kPageSize + 1, 'x');
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    REQUIRE(fs::file_size(file) == 4097);
    auto message = runtimeErrorFrom([&] { DiskManager::open(file); });
    REQUIRE(message.has_value());
    CHECK(message->find("bad.db") != std::string::npos);
}
