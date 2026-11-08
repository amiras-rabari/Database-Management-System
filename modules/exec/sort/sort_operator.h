#pragma once

// The sort operator (SPEC §M2.3): a blocking operator around ExternalSorter.
// Private to exec; plans reach it through buildExecutablePlan.

#include <memory>
#include <optional>
#include <vector>

#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/external_sort.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"

namespace db::exec
{
    /// Returns its child's tuples sorted on `keys`. Schema = the child's.
    /// open() reads the whole child (pass 0), closes it, and does every merge
    /// pass but the last; next() streams the final merge.
    ///
    /// Frames come from its FrameShare (chunk 03 phase rule): pass 0 uses
    /// ownFrames (the child streams beside it), the merge passes openFrames
    /// (the child is closed by then), the final merge streamFrames.
    class SortOperator : public Operator
    {
    public:
        /// Checks every key against the child's schema now, while the plan
        /// is built (chunk 05 Q1 (a)): unknown -> Error(Name), ambiguous ->
        /// Error(Ambiguity). No page is read.
        SortOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const std::vector<SortKey>& keys,
                     const FrameShare& share);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    public:
        /// {"passes": ..., "initial_runs": ...} of the last run (§M2.6
        /// measurement 2).
        PlanJson details() const override;

    private:
        std::vector<SortKey> keys_;
        FrameShare share_;
        /// Exists only between open and close. Each open makes a new one (a
        /// nested loop reopens its inner input); resetting it deletes its runs.
        std::optional<ExternalSorter> sorter_;
        /// The sorter's stats, kept when it is deleted at close.
        SortStats stats_;
    };
} // namespace db::exec
