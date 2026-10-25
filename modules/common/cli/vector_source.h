#pragma once

/// @file
/// A tuple source over rows held in memory (CLI and tests only).

#include "db/common/schema.h"
#include "db/common/tuple_source.h"

#include <cstddef>
#include <vector>

namespace ra
{
    /// Hands out a fixed list of rows in order; rewind() starts again.
    class VectorSource final : public db::TupleSource
    {
    public:
        /// A source of `rows`, each a tuple of `schema`.
        VectorSource(db::Schema schema, std::vector<db::Tuple> rows);

        const db::Schema& schema() const override;
        bool next(db::Tuple& out) override;
        void rewind() override;

    private:
        db::Schema schema_;
        std::vector<db::Tuple> rows_;
        std::size_t position_ = 0;
    };
}
