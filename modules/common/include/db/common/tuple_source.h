#pragma once

/// @file
/// Where operators get their tuples from: a source with a schema that hands
/// out tuples one at a time and can start again from the beginning.

#include <cstdint>
#include <memory>

#include "db/common/schema.h"

namespace db
{

    /// A stream of tuples that all have the same schema.
    ///
    /// The contract every source keeps:
    /// - schema() is fixed from construction on and stays valid as long as
    ///   the source lives.
    /// - next() writes the next tuple into `out` and returns true, or returns
    ///   false when there are no more tuples, and keeps returning false until
    ///   rewind() (the content of `out` is then unspecified).
    /// - rewind() starts the stream again: the next calls to next() produce
    ///   the same tuples in the same order once more.
    class TupleSource
    {
    public:
        /// Virtual, so a source is destroyed correctly through a TupleSourcePtr.
        virtual ~TupleSource() = default;

        /// The schema of every tuple this source produces.
        virtual const Schema& schema() const = 0;
        /// Writes the next tuple into `out` and returns true, or returns
        /// false at the end of the stream.
        virtual bool next(Tuple& out) = 0;
        /// Restarts the stream from its first tuple.
        virtual void rewind() = 0;
    };

    /// Sources are owned by the operator that reads them.
    using TupleSourcePtr = std::unique_ptr<TupleSource>;

    /// The two Bonus 1 counters, added to by the operators while they run.
    struct OperatorCounters
    {
        /// One per (left tuple, right tuple) pair a join examines.
        std::uint64_t joinComparisons = 0;
        /// One per input tuple a select examines.
        std::uint64_t selectTuples = 0;
    };

} // namespace db
