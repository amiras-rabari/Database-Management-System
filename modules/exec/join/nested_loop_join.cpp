#include "join/nested_loop_join.h"

#include <memory>
#include <utility>
#include <vector>

#include "db/common/condition.h"
#include "db/common/evaluator.h"
#include "db/common/schema.h"
#include "db/exec/frame_plan.h"
#include "db/exec/operator.h"

namespace db::exec
{
    NestedLoopJoinOperator::NestedLoopJoinOperator(ExecContext& ctx, std::unique_ptr<Operator> left,
                                                   std::unique_ptr<Operator> right, const Condition& condition,
                                                   const FrameShare& share)
        : Operator(ctx, twoChildren(std::move(left), std::move(right))),
          condition_(bindJoinCondition(condition, child(0).schema(), child(1).schema())),
          block_(ctx.pool(), child(0).schema(), share.ownFrames)
    {
        setSchema(concatSchemas(child(0).schema(), child(1).schema()));
    }

    void NestedLoopJoinOperator::doOpen()
    {
        // Only the outer is opened now. The inner is opened once per block,
        // after that block is full (it holds no frame while the block fills).
        child(0).open();
        block_.clear();
        hasCarry_ = false;
        outerDone_ = false;
        innerOpen_ = false;
        blockIndex_ = 0;
    }

    bool NestedLoopJoinOperator::fillBlock()
    {
        block_.clear();
        // The tuple left over from the last block goes first. One tuple
        // always fits in an empty block of at least one page.
        if (hasCarry_)
        {
            block_.tryAdd(carry_);
            hasCarry_ = false;
        }
        Tuple t;
        while (!outerDone_)
        {
            if (!child(0).next(t))
            {
                // Close the outer as soon as it ends: its page goes back to the pool.
                outerDone_ = true;
                child(0).close();
                break;
            }
            // tryAdd gets a copy, so `t` is still here when it does not fit.
            if (!block_.tryAdd(t))
            {
                carry_ = std::move(t);
                hasCarry_ = true;
                break;
            }
        }
        return block_.size() > 0;
    }

    bool NestedLoopJoinOperator::doNext(Tuple& out)
    {
        const std::vector<Tuple>& block = block_.tuples();
        while (true)
        {
            // No inner scan running: start the next block.
            if (!innerOpen_)
            {
                if (!fillBlock())
                    return false;
                child(1).open();
                innerOpen_ = true;
                blockIndex_ = block.size(); // no inner tuple yet: read one first
            }
            // inner_ has met every block tuple: take the next inner tuple. When
            // the inner ends, this block is done.
            if (blockIndex_ == block.size())
            {
                if (!child(1).next(inner_))
                {
                    child(1).close();
                    innerOpen_ = false;
                    continue;
                }
                blockIndex_ = 0;
            }
            // Test one pair. It counts one comparison whether it matches or not.
            const Tuple& outer = block[blockIndex_];
            ++blockIndex_;
            ++mutableCounters().comparisons;
            if (condition_.evaluate(outer, inner_))
            {
                // Output = the outer tuple's values, then the inner's (§M2.1).
                out = outer;
                out.insert(out.end(), inner_.begin(), inner_.end());
                return true;
            }
        }
    }

    void NestedLoopJoinOperator::doClose()
    {
        // close() on a child that is already closed does nothing.
        child(1).close();
        child(0).close();
        block_.clear();
        innerOpen_ = false;
    }
} // namespace db::exec
