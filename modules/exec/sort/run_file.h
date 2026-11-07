#pragma once

// Run files (chunk 06): a list of tuples written once, page by page, to a
// temporary file, then read back in the same order. Sort runs (chunk 07)
// and hash partitions (chunks 09, 13) are all stored this way.
//
// A run IS a storage::TempFile: one temp file per run (Q2 (a)). Destroying
// the TempFile deletes the file, so dropping a run leaves nothing behind
// (SPEC §M2.1). Its pages are plain slotted pages with no directory page,
// so a run of N pages costs exactly N writes and N reads.
//
// Counting (Q3 (a), D4): each full page is written to disk at once
// (flushPage: 1 write), and a finished run's pages are dropped from the
// pool (discardFile), so the next read of each page is a real disk read,
// not a cache hit. That makes measured I/O equal the spec's sort table.
//
// Each writer and each reader pins at most ONE page at a time (1 frame).
// Private to exec.

#include <cstdint>

#include "db/common/schema.h"
#include "db/storage/page.h"
#include "db/storage/page_guard.h"
#include "db/storage/storage_engine.h"
#include "db/storage/temp_file.h"

namespace db::exec
{
    /// Writes tuples to a new run. Make one, append every tuple, then call
    /// finish() once to get the run.
    class RunWriter
    {
    public:
        /// Asks the engine for a new, empty temp file. No I/O.
        /// `schema` must outlive the writer.
        RunWriter(storage::StorageEngine& engine, const Schema& schema);

        /// Adds `tuple` to the current page; when it is full, writes that
        /// page and starts a new one. A tuple too long for any page (more
        /// than 4084 encoded bytes; a wide join output can be) ->
        /// db::Error(Schema), the same rule as M1's loader (Q1 (b)).
        void append(const Tuple& tuple);

        /// Writes the last page, drops the run's pages from the pool, and
        /// hands the run over. An empty run has 0 pages and cost no I/O.
        storage::TempFile finish();

    private:
        /// Unpins the current page and writes it to disk now (1 write).
        void writeCurrentPage();

        storage::StorageEngine& engine_;
        const Schema& schema_;
        // The file is declared before the page guard on purpose: members are
        // destroyed in reverse order, so the guard unpins its page before
        // the file is deleted (a TempFile dying with a pinned page stops the
        // program). See exec STATUS.md, chunk 01 decision.
        storage::TempFile file_;
        storage::PageGuard page_; // the page being filled; empty before the first append
    };

    /// Reads a run's tuples in the order they were written. The run must
    /// outlive the reader: the reader keeps a reference to it and may hold
    /// one of its pages pinned.
    class RunReader
    {
    public:
        /// No I/O until the first next().
        RunReader(storage::StorageEngine& engine, const storage::TempFile& run, const Schema& schema);

        /// Fills `out` with the next tuple and returns true, or returns false
        /// at the end (with no page pinned). Reads each page once.
        bool next(Tuple& out);

    private:
        storage::StorageEngine& engine_;
        const storage::TempFile& run_;
        const Schema& schema_;
        storage::PageGuard page_; // the page being read; empty between pages
        storage::PageId nextPage_ = 0; // the page to pin after this one
        std::uint16_t nextSlot_ = 0; // the slot to read next on page_
    };
} // namespace db::exec
