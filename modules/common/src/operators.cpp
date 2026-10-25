#include "db/common/operators.h"

#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "db/common/evaluator.h"

namespace db
{

    namespace
    {

        TupleSourcePtr requireSource(TupleSourcePtr source, const char* operatorName)
        {
            if (!source)
            {
                throw std::logic_error(std::string(operatorName) + ": input source is null");
            }
            return source;
        }

        class SelectSource final : public TupleSource
        {
        public:
            SelectSource(TupleSourcePtr input, const Condition& condition, OperatorCounters& counters)
                : input_(std::move(input)), condition_(bindCondition(condition, input_->schema())),
                  counters_(counters)
            {
            }

            const Schema& schema() const override
            {
                return input_->schema();
            }

            bool next(Tuple& out) override
            {
                while (input_->next(out))
                {
                    ++counters_.selectTuples;
                    if (condition_.evaluate(out))
                    {
                        return true;
                    }
                }
                return false;
            }

            void rewind() override
            {
                input_->rewind();
            }

        private:
            TupleSourcePtr input_;
            BoundCondition condition_;
            OperatorCounters& counters_;
        };

        class ProjectSource final : public TupleSource
        {
        public:
            ProjectSource(TupleSourcePtr input, const std::vector<ColumnRef>& columns)
                : input_(std::move(input))
            {
                schema_ = projectSchema(input_->schema(), columns, &indices_);
            }

            const Schema& schema() const override
            {
                return schema_;
            }

            bool next(Tuple& out) override
            {
                if (!input_->next(inputTuple_))
                {
                    return false;
                }
                out.clear();
                for (std::size_t index : indices_)
                {
                    out.push_back(inputTuple_[index]);
                }
                return true;
            }

            void rewind() override
            {
                input_->rewind();
            }

        private:
            TupleSourcePtr input_;
            Schema schema_;
            std::vector<std::size_t> indices_;
            Tuple inputTuple_;
        };

        class DistinctSource final : public TupleSource
        {
        public:
            explicit DistinctSource(TupleSourcePtr input) : input_(std::move(input))
            {
            }

            const Schema& schema() const override
            {
                return input_->schema();
            }

            bool next(Tuple& out) override
            {
                while (input_->next(out))
                {
                    if (produced_.insert(out).second)
                    {
                        return true;
                    }
                }
                return false;
            }

            void rewind() override
            {
                input_->rewind();
                produced_.clear();
            }

        private:
            TupleSourcePtr input_;
            std::set<Tuple> produced_;
        };

        class RenameSource final : public TupleSource
        {
        public:
            RenameSource(TupleSourcePtr input, const std::string& alias)
                : input_(std::move(input)), schema_(requalify(input_->schema(), alias))
            {
            }

            const Schema& schema() const override
            {
                return schema_;
            }

            bool next(Tuple& out) override
            {
                return input_->next(out);
            }

            void rewind() override
            {
                input_->rewind();
            }

        private:
            TupleSourcePtr input_;
            Schema schema_;
        };

        // Cross product and join: the same nested loop, with or without a
        // condition. Only a join counts its comparisons.
        class NestedLoopSource final : public TupleSource
        {
        public:
            NestedLoopSource(TupleSourcePtr left, TupleSourcePtr right, const Condition* condition,
                             OperatorCounters* counters)
                : left_(std::move(left)), right_(std::move(right)),
                  schema_(concatSchemas(left_->schema(), right_->schema())), counters_(counters)
            {
                if (condition != nullptr)
                {
                    condition_ = bindJoinCondition(*condition, left_->schema(), right_->schema());
                }
            }

            const Schema& schema() const override
            {
                return schema_;
            }

            bool next(Tuple& out) override
            {
                while (true)
                {
                    if (!haveLeft_)
                    {
                        if (!left_->next(leftTuple_))
                        {
                            return false;
                        }
                        haveLeft_ = true;
                        right_->rewind();
                    }
                    while (right_->next(rightTuple_))
                    {
                        if (counters_ != nullptr)
                        {
                            ++counters_->joinComparisons;
                        }
                        if (!condition_ || condition_->evaluate(leftTuple_, rightTuple_))
                        {
                            out = leftTuple_;
                            out.insert(out.end(), rightTuple_.begin(), rightTuple_.end());
                            return true;
                        }
                    }
                    haveLeft_ = false;
                }
            }

            void rewind() override
            {
                left_->rewind();
                haveLeft_ = false;
            }

        private:
            TupleSourcePtr left_;
            TupleSourcePtr right_;
            Schema schema_;
            std::optional<BoundCondition> condition_;
            OperatorCounters* counters_;
            bool haveLeft_ = false;
            Tuple leftTuple_;
            Tuple rightTuple_;
        };

        class UnionSource final : public TupleSource
        {
        public:
            UnionSource(TupleSourcePtr left, TupleSourcePtr right)
                : left_(std::move(left)), right_(std::move(right))
            {
                checkUnionCompatible(left_->schema(), right_->schema(), "union");
            }

            const Schema& schema() const override
            {
                return left_->schema();
            }

            bool next(Tuple& out) override
            {
                if (!leftDone_)
                {
                    while (left_->next(out))
                    {
                        if (produced_.insert(out).second)
                        {
                            return true;
                        }
                    }
                    leftDone_ = true;
                }
                while (right_->next(out))
                {
                    if (produced_.insert(out).second)
                    {
                        return true;
                    }
                }
                return false;
            }

            void rewind() override
            {
                left_->rewind();
                right_->rewind();
                leftDone_ = false;
                produced_.clear();
            }

        private:
            TupleSourcePtr left_;
            TupleSourcePtr right_;
            bool leftDone_ = false;
            std::set<Tuple> produced_;
        };

        // Intersect keeps the left tuples found in the right input, except
        // keeps the ones not found there.
        class SetFilterSource final : public TupleSource
        {
        public:
            SetFilterSource(TupleSourcePtr left, TupleSourcePtr right, bool keepIfInRight,
                            const char* operatorName)
                : left_(std::move(left)), right_(std::move(right)), keepIfInRight_(keepIfInRight)
            {
                checkUnionCompatible(left_->schema(), right_->schema(), operatorName);
            }

            const Schema& schema() const override
            {
                return left_->schema();
            }

            bool next(Tuple& out) override
            {
                if (!rightLoaded_)
                {
                    Tuple tuple;
                    while (right_->next(tuple))
                    {
                        rightTuples_.insert(tuple);
                    }
                    rightLoaded_ = true;
                }
                while (left_->next(out))
                {
                    const bool inRight = rightTuples_.count(out) > 0;
                    if (inRight == keepIfInRight_ && produced_.insert(out).second)
                    {
                        return true;
                    }
                }
                return false;
            }

            void rewind() override
            {
                left_->rewind();
                right_->rewind();
                rightLoaded_ = false;
                rightTuples_.clear();
                produced_.clear();
            }

        private:
            TupleSourcePtr left_;
            TupleSourcePtr right_;
            bool keepIfInRight_;
            bool rightLoaded_ = false;
            std::set<Tuple> rightTuples_;
            std::set<Tuple> produced_;
        };

        void requireShape(const PlanNode& plan, std::size_t children, bool needsCondition)
        {
            if (plan.children.size() != children)
            {
                throw std::logic_error("plan node '" + std::string(opName(plan.op)) + "' has " +
                                       std::to_string(plan.children.size()) + " children");
            }
            if (needsCondition && !plan.condition)
            {
                throw std::logic_error("plan node '" + std::string(opName(plan.op)) + "' has no condition");
            }
        }

        bool isUnary(PlanOp op)
        {
            return op == PlanOp::Select || op == PlanOp::Project || op == PlanOp::Rename ||
                   op == PlanOp::Distinct;
        }

        TupleSourcePtr openScan(const PlanNode& plan, const OpenTable& openTable)
        {
            requireShape(plan, 0, false);
            TupleSourcePtr source = openTable(plan.table);
            if (!source)
            {
                throw std::logic_error("openTable returned no source for '" + plan.table + "'");
            }
            return source;
        }

        // The operator of a unary node over its already built input. Kept out
        // of buildOperators so the recursion's own frames stay small.
        TupleSourcePtr applyUnary(const PlanNode& plan, TupleSourcePtr input, OperatorCounters& counters)
        {
            switch (plan.op)
            {
                case PlanOp::Select:
                    return select(std::move(input), *plan.condition, counters);
                case PlanOp::Project:
                    return project(std::move(input), plan.columns);
                case PlanOp::Rename:
                    return rename(std::move(input), plan.alias);
                case PlanOp::Distinct:
                    return distinct(std::move(input));
                default:
                    throw std::logic_error("applyUnary: not a unary PlanOp");
            }
        }

        TupleSourcePtr applyBinary(const PlanNode& plan, TupleSourcePtr left, TupleSourcePtr right,
                                   OperatorCounters& counters)
        {
            switch (plan.op)
            {
                case PlanOp::Join:
                    return join(std::move(left), std::move(right), *plan.condition, counters);
                case PlanOp::Cross:
                    return cross(std::move(left), std::move(right));
                case PlanOp::Union:
                    return unionOf(std::move(left), std::move(right));
                case PlanOp::Intersect:
                    return intersect(std::move(left), std::move(right));
                case PlanOp::Except:
                    return except(std::move(left), std::move(right));
                default:
                    throw std::logic_error("applyBinary: not a binary PlanOp");
            }
        }

    } // namespace

    TupleSourcePtr select(TupleSourcePtr input, const Condition& condition, OperatorCounters& counters)
    {
        return std::make_unique<SelectSource>(requireSource(std::move(input), "select"), condition, counters);
    }

    TupleSourcePtr project(TupleSourcePtr input, const std::vector<ColumnRef>& columns)
    {
        return std::make_unique<ProjectSource>(requireSource(std::move(input), "project"), columns);
    }

    TupleSourcePtr distinct(TupleSourcePtr input)
    {
        return std::make_unique<DistinctSource>(requireSource(std::move(input), "distinct"));
    }

    TupleSourcePtr rename(TupleSourcePtr input, const std::string& alias)
    {
        return std::make_unique<RenameSource>(requireSource(std::move(input), "rename"), alias);
    }

    TupleSourcePtr cross(TupleSourcePtr left, TupleSourcePtr right)
    {
        return std::make_unique<NestedLoopSource>(requireSource(std::move(left), "cross"),
                                                  requireSource(std::move(right), "cross"), nullptr, nullptr);
    }

    TupleSourcePtr join(TupleSourcePtr left, TupleSourcePtr right, const Condition& condition,
                        OperatorCounters& counters)
    {
        return std::make_unique<NestedLoopSource>(requireSource(std::move(left), "join"),
                                                  requireSource(std::move(right), "join"), &condition, &counters);
    }

    TupleSourcePtr unionOf(TupleSourcePtr left, TupleSourcePtr right)
    {
        return std::make_unique<UnionSource>(requireSource(std::move(left), "union"),
                                             requireSource(std::move(right), "union"));
    }

    TupleSourcePtr intersect(TupleSourcePtr left, TupleSourcePtr right)
    {
        return std::make_unique<SetFilterSource>(requireSource(std::move(left), "intersect"),
                                                 requireSource(std::move(right), "intersect"), true, "intersect");
    }

    TupleSourcePtr except(TupleSourcePtr left, TupleSourcePtr right)
    {
        return std::make_unique<SetFilterSource>(requireSource(std::move(left), "except"),
                                                 requireSource(std::move(right), "except"), false, "except");
    }

    TupleSourcePtr buildOperators(const PlanNode& plan, const OpenTable& openTable, OperatorCounters& counters)
    {
        if (plan.op == PlanOp::Scan)
        {
            return openScan(plan, openTable);
        }
        const bool unary = isUnary(plan.op);
        requireShape(plan, unary ? 1 : 2, plan.op == PlanOp::Select || plan.op == PlanOp::Join);
        TupleSourcePtr first = buildOperators(plan.children[0], openTable, counters);
        if (unary)
        {
            return applyUnary(plan, std::move(first), counters);
        }
        TupleSourcePtr second = buildOperators(plan.children[1], openTable, counters);
        return applyBinary(plan, std::move(first), std::move(second), counters);
    }

} // namespace db
