#include "join/sort_merge_join.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "db/common/plan.h"
#include "db/storage/storage_engine.h"
#include "sort/set_ops.h"

namespace db::exec
{
    namespace
    {
        // Pages for the run of equal right keys; a longer run is spilled.
        constexpr std::size_t kGroupPages = 1;
    } // namespace

    SortMergeJoinOperator::SortMergeJoinOperator(ExecContext& ctx, std::unique_ptr<Operator> left,
                                                 std::unique_ptr<Operator> right, const Condition& condition,
                                                 const FrameShare& share)
        : Operator(ctx, twoChildren(std::move(left), std::move(right))),
          keys_(equiJoinKeyColumns(condition, child(0).schema(), child(1).schema(), "sort_merge_join")),
          share_(share), group_(ctx.pool(), child(1).schema(), kGroupPages)
    {
        setSchema(concatSchemas(child(0).schema(), child(1).schema()));
    }

    void SortMergeJoinOperator::doOpen()
    {
        // Sort the left input, then the right one: while the right is being
        // sorted, the left is only runs on disk and holds no frame. Then the
        // stream share is split: 1 page for the group, the rest between the
        // two final merges (one frame per run read, at least 1 each).
        const std::size_t readers = share_.streamFrames - kGroupPages;
        const std::size_t rightReaders = readers / 2;
        const std::size_t leftReaders = readers - rightReaders;
        std::uint64_t& comparisons = mutableCounters().comparisons;
        leftSorter_ = sortToRuns(ctx().engine(), child(0), ascendingKeys(child(0).schema(), keys_.leftColumns),
                                 share_, leftReaders + 1, comparisons);
        rightSorter_ = sortToRuns(ctx().engine(), child(1), ascendingKeys(child(1).schema(), keys_.rightColumns),
                                  share_, rightReaders + 1, comparisons);
        leftSorter_->startFinalMerge();
        rightSorter_->startFinalMerge();
        hasLeft_ = leftSorter_->next(leftHead_);
        hasRight_ = rightSorter_->next(rightHead_);
        emitting_ = false;
    }

    bool SortMergeJoinOperator::doNext(Tuple& out)
    {
        std::uint64_t& comparisons = mutableCounters().comparisons;
        while (true)
        {
            if (emitting_)
            {
                // Pair leftHead_ with the next tuple of the group.
                Tuple s;
                if (nextInGroup(s))
                {
                    out = leftHead_;
                    out.insert(out.end(), s.begin(), s.end());
                    return true;
                }
                // The group is done for this r'. If the next r has the same
                // key, rewind the group for it; else the run of R is over.
                hasLeft_ = leftSorter_->next(leftHead_);
                if (hasLeft_ &&
                    compareKeys(leftHead_, keys_.leftColumns, groupKey_, keys_.rightColumns, comparisons) == 0)
                {
                    rewindGroup();
                    continue;
                }
                endGroup();
                emitting_ = false;
                continue;
            }

            if (!hasLeft_ || !hasRight_)
                return false;
            const int order = compareKeys(leftHead_, keys_.leftColumns, rightHead_, keys_.rightColumns, comparisons);
            if (order < 0)
                hasLeft_ = leftSorter_->next(leftHead_);
            else if (order > 0)
                hasRight_ = rightSorter_->next(rightHead_);
            else
            {
                collectGroup();
                emitting_ = true;
            }
        }
    }

    void SortMergeJoinOperator::collectGroup()
    {
        // The group is every right tuple with rightHead_'s key; the right
        // input is left on the first tuple after it ("advance s past its run").
        std::uint64_t& comparisons = mutableCounters().comparisons;
        groupKey_ = rightHead_;
        addToGroup(rightHead_);
        while (true)
        {
            hasRight_ = rightSorter_->next(rightHead_);
            if (!hasRight_ ||
                compareKeys(rightHead_, keys_.rightColumns, groupKey_, keys_.rightColumns, comparisons) != 0)
                break;
            addToGroup(rightHead_);
        }
        if (groupWriter_)
        {
            groupRun_ = groupWriter_->finish();
            groupWriter_.reset();
        }
        rewindGroup();
    }

    void SortMergeJoinOperator::addToGroup(const Tuple& t)
    {
        if (!groupWriter_ && group_.tryAdd(t))
            return;
        if (!groupWriter_)
        {
            // The group outgrew its page: spill it. The writer's page is the
            // group's own frame, handed over first, so the frames held do
            // not grow; the kept tuples move onto it one by one. Their order
            // changes, which does not matter: the output is a bag.
            group_.giveFrameToOutput();
            groupWriter_ = std::make_unique<RunWriter>(ctx().engine(), child(1).schema());
            while (group_.size() > 0)
            {
                groupWriter_->append(group_.tuples().back());
                group_.popBack();
            }
        }
        groupWriter_->append(t);
    }

    void SortMergeJoinOperator::rewindGroup()
    {
        groupIndex_ = 0;
        if (groupRun_)
        {
            // A spilled group is read again from its first page: one read
            // per page for every r' of the run (pages of a finished run are
            // not kept in the pool).
            groupReader_.reset(); // unpin first, so only one reader page is held
            groupReader_ = std::make_unique<RunReader>(ctx().engine(), *groupRun_, child(1).schema());
        }
    }

    bool SortMergeJoinOperator::nextInGroup(Tuple& out)
    {
        if (groupRun_)
            return groupReader_->next(out);
        if (groupIndex_ == group_.size())
            return false;
        out = group_.tuples()[groupIndex_];
        ++groupIndex_;
        return true;
    }

    void SortMergeJoinOperator::endGroup()
    {
        groupReader_.reset();
        groupRun_.reset();
        group_.clear();
        groupIndex_ = 0;
    }

    void SortMergeJoinOperator::doClose()
    {
        endGroup();
        groupWriter_.reset();
        emitting_ = false;
        // Destroying a sorter closes its readers, then deletes its runs.
        leftSorter_.reset();
        rightSorter_.reset();
    }
} // namespace db::exec
