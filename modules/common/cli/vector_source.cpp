#include "vector_source.h"

#include <utility>

namespace ra
{
    VectorSource::VectorSource(db::Schema schema, std::vector<db::Tuple> rows)
        : schema_(std::move(schema)), rows_(std::move(rows))
    {
    }

    const db::Schema& VectorSource::schema() const
    {
        return schema_;
    }

    bool VectorSource::next(db::Tuple& out)
    {
        if (position_ >= rows_.size())
        {
            return false;
        }
        out = rows_[position_];
        ++position_;
        return true;
    }

    void VectorSource::rewind()
    {
        position_ = 0;
    }
}
