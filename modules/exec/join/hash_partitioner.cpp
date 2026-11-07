#include "join/hash_partitioner.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "join/hashing.h"

namespace db::exec
{
    HashPartitioner::HashPartitioner(storage::StorageEngine& engine, const Schema& schema,
                                     std::vector<std::size_t> keyColumns, std::size_t partitions, std::uint64_t seed)
        : keyColumns_(std::move(keyColumns)), seed_(seed)
    {
        // Each writer makes its temp file now but pins no page until its
        // first tuple. reserve: the vector never moves a writer.
        writers_.reserve(partitions);
        for (std::size_t i = 0; i < partitions; ++i)
            writers_.emplace_back(engine, schema);
    }

    void HashPartitioner::add(const Tuple& tuple)
    {
        writers_[hashKey(tuple, keyColumns_, seed_) % writers_.size()].append(tuple);
    }

    std::vector<storage::TempFile> HashPartitioner::finish()
    {
        std::vector<storage::TempFile> runs;
        for (RunWriter& writer : writers_)
            runs.push_back(writer.finish());
        return runs;
    }
} // namespace db::exec
