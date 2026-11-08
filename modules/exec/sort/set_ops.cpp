#include "sort/set_ops.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "db/common/plan.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/external_sort.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"
#include "db/storage/storage_engine.h"

namespace db::exec
{
    // Sort keys for every column, in order, ascending. Only the column
    // order matters, not the direction: equal tuples end up next to each
    // other either way. Qualified names are unique in a schema, so each
    // key names exactly one column.
    std::vector<SortKey> allColumnsAscending(const Schema& schema)
    {
        return firstColumnsAscending(schema, schema.size());
    }

    std::vector<SortKey> firstColumnsAscending(const Schema& schema, std::size_t count)
    {
        std::vector<SortKey> keys;
        for (std::size_t i = 0; i < count; ++i)
        {
            const Column& column = schema.column(i);
            keys.push_back(SortKey{ColumnRef{column.qualifier, column.name}, true});
        }
        return keys;
    }

    // Compares two tuples column by column, by position (the inputs of a
    // set operation may have different names; their types already match).
    // Negative if `a` comes first, 0 if equal, positive if `b` comes
    // first: the same order as allColumnsAscending. Counts 1 comparison,
    // like the sorter's comparator (chunk 07 Q2 (a)).
    int compareRows(const Tuple& a, const Tuple& b, std::uint64_t& comparisons)
    {
        return compareFirstColumns(a, b, a.size(), comparisons);
    }

    int compareFirstColumns(const Tuple& a, const Tuple& b, std::size_t count, std::uint64_t& comparisons)
    {
        ++comparisons;
        for (std::size_t i = 0; i < count; ++i)
        {
            const int result = compareValues(a[i], b[i]);
            if (result != 0)
                return result;
        }
        return 0;
    }

    namespace
    {
        // The left schema with each VARCHAR length raised to the right
        // input's if that is longer. Example: VARCHAR(3) and VARCHAR(10)
        // give VARCHAR(10), so "longer" from the right fits on a run page.
        Schema widerSchema(const Schema& left, const Schema& right)
        {
            std::vector<Column> columns = left.columns();
            for (std::size_t i = 0; i < columns.size(); ++i)
                columns[i].varcharLength = std::max(columns[i].varcharLength, right.column(i).varcharLength);
            return Schema(columns);
        }
    } // namespace

    // ---- distinct (sort mode) and union ----

    DedupSortOperator::DedupSortOperator(ExecContext& ctx, Children children, const FrameShare& share)
        : Operator(ctx, std::move(children)), share_(share)
    {
        sortSchema_ = child(0).schema();
        if (this->children().size() == 2)
        {
            checkUnionCompatible(child(0).schema(), child(1).schema(), "union");
            sortSchema_ = widerSchema(child(0).schema(), child(1).schema());
        }
        setSchema(child(0).schema());
    }

    void DedupSortOperator::doOpen()
    {
        sorter_.emplace(ctx().engine(), sortSchema_, allColumnsAscending(sortSchema_), share_.ownFrames,
                        share_.openFrames, share_.streamFrames, mutableCounters().comparisons);
        hasLast_ = false;

        // Pass 0 over each input in turn. Each child is opened only when the
        // one before it is closed (chunk 03 runtime rule 1, like union_all).
        for (std::size_t i = 0; i < children().size(); ++i)
        {
            // Union: the right input may need every frame while it opens (a
            // join or a sort below it), so the left input's tuples still in
            // pass 0's buffer are written as a run first.
            if (i > 0)
                sorter_->writeBufferedRun();
            child(i).open();
            Tuple t;
            while (child(i).next(t))
                sorter_->add(t);
            child(i).close();
        }
        // Every merge pass but the last (§M2.3: inside open).
        sorter_->finishInput();
    }

    bool DedupSortOperator::doNext(Tuple& out)
    {
        // The input comes out sorted, so all copies of a tuple are next to
        // each other: skipping a tuple equal to the last one returned drops
        // every duplicate. Example: 1 1 2 3 3 3 gives 1 2 3.
        Tuple t;
        while (sorter_->next(t))
        {
            if (hasLast_ && compareRows(t, last_, mutableCounters().comparisons) == 0)
                continue; // a copy of the tuple returned last
            last_ = t;
            hasLast_ = true;
            out = std::move(t);
            return true;
        }
        return false;
    }

    void DedupSortOperator::doClose()
    {
        // Deletes every run left and gives back the sorter's frames. The
        // children were already closed in doOpen.
        sorter_.reset();
    }

    // ---- intersect and except ----

    SetMergeOperator::SetMergeOperator(ExecContext& ctx, std::unique_ptr<Operator> left,
                                       std::unique_ptr<Operator> right, Kind kind, const FrameShare& share)
        : Operator(ctx, twoChildren(std::move(left), std::move(right))), kind_(kind), share_(share)
    {
        checkUnionCompatible(child(0).schema(), child(1).schema(), kind == Kind::Intersect ? "intersect" : "except");
        setSchema(child(0).schema());
    }

    std::unique_ptr<ExternalSorter> sortToRuns(storage::StorageEngine& engine, Operator& input,
                                               const std::vector<SortKey>& keys, const FrameShare& share,
                                               std::size_t finalFrames, std::uint64_t& comparisons)
    {
        auto sorter = std::make_unique<ExternalSorter>(engine, input.schema(), keys, share.ownFrames,
                                                       share.openFrames, finalFrames, comparisons);
        input.open();
        Tuple t;
        while (input.next(t))
            sorter->add(t);
        input.close();
        sorter->finishToRuns();
        return sorter;
    }

    void SetMergeOperator::doOpen()
    {
        // Left first, then right: while the right input is sorted, the left
        // one is only runs on disk and holds no frame (phase rule). Each
        // input is sorted with its own schema: only positions are compared,
        // so names and VARCHAR lengths may differ. The stream share is split
        // between the two final merges, one frame per run read.
        const std::size_t leftReaders = share_.streamFrames / 2;
        const std::size_t rightReaders = share_.streamFrames - leftReaders;
        std::uint64_t& comparisons = mutableCounters().comparisons;
        leftSorter_ = sortToRuns(ctx().engine(), child(0), allColumnsAscending(child(0).schema()), share_,
                                 leftReaders + 1, comparisons);
        rightSorter_ = sortToRuns(ctx().engine(), child(1), allColumnsAscending(child(1).schema()), share_,
                                  rightReaders + 1, comparisons);
        leftSorter_->startFinalMerge();
        rightSorter_->startFinalMerge();
        hasLeft_ = leftSorter_->next(leftHead_);
        hasRight_ = rightSorter_->next(rightHead_);
    }

    void SetMergeOperator::skipLeft(const Tuple& value)
    {
        do
            hasLeft_ = leftSorter_->next(leftHead_);
        while (hasLeft_ && compareRows(leftHead_, value, mutableCounters().comparisons) == 0);
    }

    void SetMergeOperator::skipRight(const Tuple& value)
    {
        do
            hasRight_ = rightSorter_->next(rightHead_);
        while (hasRight_ && compareRows(rightHead_, value, mutableCounters().comparisons) == 0);
    }

    bool SetMergeOperator::doNext(Tuple& out)
    {
        // Both runs are sorted, so the smaller head can never match anything
        // further on the other side. Example, intersect [1 1 2 3] and
        // [1 3 3 4]: 1 = 1 -> return 1, skip both past 1; 2 < 3 -> skip 2;
        // 3 = 3 -> return 3; left is used up -> end. Result {1, 3}.
        while (hasLeft_)
        {
            // Right used up: for except, every remaining left value counts.
            const int order = hasRight_ ? compareRows(leftHead_, rightHead_, mutableCounters().comparisons) : -1;
            if (order < 0)
            {
                // The left head is not on the right.
                const Tuple value = leftHead_;
                skipLeft(value);
                if (kind_ == Kind::Except)
                {
                    out = value;
                    return true;
                }
                if (!hasRight_)
                    return false; // intersect: nothing left can match
            }
            else if (order > 0)
            {
                // The right head is not on the left: it cannot matter.
                const Tuple value = rightHead_;
                skipRight(value);
            }
            else
            {
                // On both sides: skip it on both, return it once for intersect.
                const Tuple value = leftHead_;
                skipLeft(value);
                skipRight(value);
                if (kind_ == Kind::Intersect)
                {
                    out = value;
                    return true;
                }
            }
        }
        return false;
    }

    void SetMergeOperator::doClose()
    {
        // Destroying a sorter closes its readers, then deletes its runs.
        leftSorter_.reset();
        rightSorter_.reset();
    }
} // namespace db::exec
