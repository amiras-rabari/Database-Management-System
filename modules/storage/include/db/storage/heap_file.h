#pragma once

/// @file
/// The heap file of §M1.5: one table's `<table>.db`, seen as a linked list
/// of directory pages that list the table's data pages (slotted pages) and
/// how many bytes each one still has free.
///
/// Page order in the file. Pages are only ever added at the end, in a fixed
/// order: page 0 is the first directory page, then its data pages; when a
/// directory page is full (681 entries) a new directory page is added
/// *before* the next data page. So the file always looks like:
///
/// ```
/// page   0      directory page, lists data pages 1 .. 681
/// pages  1..681 data pages
/// page   682    directory page, lists data pages 683 .. 1363
/// pages  683..  data pages ...
/// ```
/// Directory pages are exactly the multiples of kDirStride = 682 (1 directory
/// page + 681 data pages). Data page p is listed on directory page
/// (p / 682) * 682, at entry p - that page - 1. This lets get/remove find
/// things without walking the directory chain (DESIGN_M1.md states it).

#include <cstdint>
#include <string>

#include "db/common/schema.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/directory_page.h"
#include "db/storage/file_registry.h"
#include "db/storage/heap_scan.h"
#include "db/storage/page.h"
#include "db/storage/rid.h"

namespace db::storage
{
    /// One directory page plus the 681 data pages it lists.
    inline constexpr PageId kDirStride = 1 + kDirEntriesPerPage;

    /// A handle on one table's heap file. It holds no pins between calls:
    /// every operation pins the pages it needs through PageGuards and has
    /// released them all when it returns (or throws). At most two pages are
    /// pinned at any moment (one directory page + one data page), so every
    /// operation works with B = 2.
    ///
    /// Copying a HeapFile copies a pointer and a few names, nothing else
    /// (rule of zero: no member needs special care). The pool must outlive
    /// every HeapFile that uses it (the storage engine guarantees it, chunk 12).
    ///
    /// Errors (D1):
    /// - a record longer than 4084 bytes (it can never fit, D5) -> db::Error(Schema)
    ///   naming the table and the row; nothing is changed;
    /// - a rid that names no live record (not a data page of this file,
    ///   no such slot, or a deleted slot) -> db::Error(Name) naming the table and the rid;
    /// - tuple values that do not match the schema -> the codec's Error(Schema/Type);
    /// - a directory that disagrees with the pages (corrupt file) -> std::runtime_error.
    class HeapFile
    {
    public:
        /// Turns an empty file into an empty heap file: adds page 0 and
        /// formats it as a directory page with no entries and no next page.
        /// Throws std::logic_error if the file already has pages.
        static void create(BufferPool& pool, FileId fileId);

        /// Opens the heap file in `fileId` (made by create). `schema` is the
        /// table's schema and `tableName` is used in error messages. Reads nothing.
        HeapFile(BufferPool& pool, FileId fileId, Schema schema, std::string tableName);

        /// Stores `tuple` and returns its rid. First fit (§M1.5): walks the
        /// directory pages in order and uses the first data page whose free
        /// bytes are >= L + 4. If none fits, adds a new data page (and first a
        /// new directory page if the last one is full).
        Rid insert(const Tuple& tuple);

        /// The record at `rid`, decoded. One data page access (§M1.5 allows two).
        Tuple get(Rid rid) const;

        /// Deletes the record at `rid` (its slot becomes dead; the rid is
        /// never used again) and writes the page's free space into its
        /// directory entry (§M1.5). No compaction (D3), so the free space
        /// does not change.
        void remove(Rid rid);

        /// A scan over every live record, in directory order then slot order
        /// (§M1.5). Pins nothing until its first next() (lazy start); then
        /// at most one directory page and one data page at a time.
        HeapScan scan() const;

        /// Number of data pages (directory pages not counted, S11), from the
        /// file's page count alone: no page access.
        std::uint32_t numDataPages() const;

        FileId fileId() const { return fileId_; }
        const Schema& schema() const { return schema_; }
        const std::string& tableName() const { return tableName_; }

    private:
        /// Throws Error(Name) unless `rid.pageId` is a data page of this file.
        void requireDataPage(Rid rid) const;

        /// Pointer, not reference, so a HeapFile can be copied and assigned.
        BufferPool* pool_;
        FileId fileId_;
        Schema schema_;
        std::string tableName_;
    };
} // namespace db::storage
