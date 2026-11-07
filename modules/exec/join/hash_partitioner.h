#pragma once

// Splits tuples into P partitions on disk by a hash of their key (chunk 09;
// SPEC §M2.3: "write each tuple to partition h1(key) mod P"). Hash distinct
// uses it now; hash group_by (chunk 10) and Grace hash join (chunk 13) later.
// Private to exec.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "db/common/schema.h"
#include "db/storage/storage_engine.h"
#include "db/storage/temp_file.h"
#include "sort/run_file.h"

namespace db::exec
{
    /// One run writer per partition. Each writer pins at most one page, so
    /// the partitioner holds at most P frames, however big a partition grows:
    /// a full page is written to disk at once (run files, chunk 06). Equal
    /// keys always go to the same partition.
    class HashPartitioner
    {
    public:
        /// Makes P empty partitions (P >= 1). No I/O yet. `schema` must
        /// outlive the partitioner.
        HashPartitioner(storage::StorageEngine& engine, const Schema& schema, std::vector<std::size_t> keyColumns,
                        std::size_t partitions, std::uint64_t seed);

        /// Writes `tuple` to partition hashKey(key, seed) mod P.
        void add(const Tuple& tuple);

        /// Writes each partition's last page and hands the P runs over, in
        /// partition order (an empty partition is a run of 0 pages). Call once.
        std::vector<storage::TempFile> finish();

    private:
        std::vector<std::size_t> keyColumns_;
        std::uint64_t seed_;
        std::vector<RunWriter> writers_;
    };
} // namespace db::exec
