#include "sort/sort_operator.h"

#include <utility>

namespace db::exec
{
    SortOperator::SortOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const std::vector<SortKey>& keys,
                               const FrameShare& share)
        : Operator(ctx, oneChild(std::move(child))), keys_(keys), share_(share)
    {
        // The check of chunk 05 Q1 (a): resolve throws for a bad name. The
        // sorter resolves the keys again at open, where they can no longer fail.
        for (const SortKey& key : keys_)
            this->child(0).schema().resolve(key.column);
        setSchema(this->child(0).schema());
    }

    void SortOperator::doOpen()
    {
        // Comparisons go straight into this node's counter, so they add up
        // over several opens.
        sorter_.emplace(ctx().engine(), schema(), keys_, share_.ownFrames, share_.openFrames, share_.streamFrames,
                        mutableCounters().comparisons);

        // Pass 0. The child is opened only now and closed as soon as it is
        // used up (chunk 03 runtime rule 1), so the merge passes have its
        // frames too.
        child(0).open();
        Tuple t;
        while (child(0).next(t))
            sorter_->add(t);
        child(0).close();

        // Every merge pass but the last (§M2.3: they happen inside open).
        sorter_->finishInput();
    }

    bool SortOperator::doNext(Tuple& out)
    {
        return sorter_->next(out);
    }

    void SortOperator::doClose()
    {
        // Deletes every run that is left and gives back the sorter's frames.
        // The child was already closed in doOpen. (If open stops with an
        // error, close is never reached; destroying the plan then releases
        // everything, chunk 01 RAII rule.)
        if (sorter_)
            stats_ = sorter_->stats();
        sorter_.reset();
    }

    PlanJson SortOperator::details() const
    {
        PlanJson details = PlanJson::object();
        details["passes"] = stats_.passes;
        details["initial_runs"] = stats_.initialRuns;
        return details;
    }
} // namespace db::exec
