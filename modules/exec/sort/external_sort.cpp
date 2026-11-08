#include "db/exec/external_sort.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "db/common/value.h"
#include "sort/run_file.h"

namespace db::exec
{
    std::size_t expectedSortPasses(std::size_t pages, std::size_t frames)
    {
        // Pass 0 makes ceil(N / B) runs.
        std::size_t runs = (pages + frames - 1) / frames;
        std::size_t passes = 1;
        // Each merge pass merges B - 1 runs into one: runs = ceil(runs / (B - 1)).
        // Example N = 80, B = 4: 20 -> 7 -> 3 -> 1, so 1 + 3 = 4 passes.
        while (runs > 1)
        {
            runs = (runs + frames - 2) / (frames - 1);
            ++passes;
        }
        return passes;
    }

    ExternalSorter::ExternalSorter(storage::StorageEngine& engine, const Schema& schema,
                                   const std::vector<SortKey>& keys, std::size_t runFrames, std::size_t mergeFrames,
                                   std::size_t finalFrames, std::uint64_t& comparisons)
        : engine_(engine), schema_(schema), keys_(keys), mergeFrames_(mergeFrames), finalFrames_(finalFrames),
          comparisons_(comparisons), buffer_(engine.pool(), schema, runFrames)
    {
        // Names are turned into column positions once, so a comparison does
        // no string work. resolve throws for an unknown or ambiguous name.
        for (const SortKey& key : keys_)
            keyColumns_.push_back(schema_.resolve(key.column));
    }

    // Here, not in the header: destroying the readers needs the full
    // RunReader type.
    ExternalSorter::~ExternalSorter() = default;

    int ExternalSorter::compareTuples(const Tuple& a, const Tuple& b)
    {
        // One call is one comparison (Q2 (a)), however many keys it looks at.
        ++comparisons_;
        // Keys in order: the first key that differs decides. Both values of a
        // key come from the same column, so they always have the same type.
        for (std::size_t k = 0; k < keys_.size(); ++k)
        {
            const std::size_t column = keyColumns_[k];
            const int result = compareValues(a[column], b[column]);
            if (result != 0)
                return keys_[k].ascending ? result : -result;
        }
        return 0;
    }

    void ExternalSorter::add(const Tuple& tuple)
    {
        // tryAdd keeps its own copy, so `tuple` is still here if it is refused.
        if (buffer_.tryAdd(tuple))
            return;
        // The buffer is full: write it out as a run, then start the next run
        // with this tuple (an empty buffer always takes one tuple).
        writeBufferAsRun();
        buffer_.tryAdd(tuple);
    }

    void ExternalSorter::sortBufferLargestFirst()
    {
        // Largest first, so the smallest tuple is at the back, where popBack
        // takes tuples from. std::sort is allowed here: it sorts one run that
        // fits in memory (§0.2), never the whole input.
        std::vector<Tuple>& tuples = buffer_.tuples();
        std::sort(tuples.begin(), tuples.end(),
                  [this](const Tuple& a, const Tuple& b) { return compareTuples(a, b) > 0; });
    }

    void ExternalSorter::writeBufferAsRun()
    {
        sortBufferLargestFirst();
        // The page the run is written to is one of the run's frames (Q3 (a)).
        // Each tuple is copied onto the page and then leaves the buffer, so
        // the buffer gives frames back as fast as the writer fills pages.
        buffer_.giveFrameToOutput();
        RunWriter writer(engine_, schema_);
        while (buffer_.size() > 0)
        {
            writer.append(buffer_.tuples().back());
            buffer_.popBack();
        }
        runs_.push_back(writer.finish());
    }

    void ExternalSorter::writeBufferedRun()
    {
        if (buffer_.size() > 0)
            writeBufferAsRun();
    }

    void ExternalSorter::finishInput()
    {
        stats_.passes = 1; // pass 0

        // Nothing written yet, and the whole input also fits in the frames of
        // the final pass: one run, one pass, nothing written. It is streamed
        // straight from memory. (If it fits pass 0's frames but not the final
        // pass's, a sort below another operator, it is written like any run.)
        if (runs_.empty() && buffer_.pagesUsed() <= finalFrames_)
        {
            stats_.initialRuns = buffer_.size() > 0 ? 1 : 0;
            sortBufferLargestFirst();
            inMemory_ = true;
            return;
        }

        if (buffer_.size() > 0)
            writeBufferAsRun(); // the last, shorter run
        stats_.initialRuns = runs_.size();

        // Merge passes happen here, inside open (§M2.3), until the final
        // merge can take every run at once: one frame per run, and the spec
        // keeps 1 frame for output in every merge, so finalFrames - 1 runs.
        while (runs_.size() > finalFrames_ - 1)
        {
            mergePass(mergeFrames_ - 1);
            ++stats_.passes;
        }

        // The final pass merges while next() is called; it writes nothing.
        startMerge(0, runs_.size());
        ++stats_.passes;
    }

    void ExternalSorter::finishToRuns()
    {
        stats_.passes = 1; // pass 0
        // No in-memory shortcut: the caller is about to give its frames to
        // its other input, so the sorted data must be on disk.
        if (buffer_.size() > 0)
            writeBufferAsRun();
        stats_.initialRuns = runs_.size();

        // The same merge passes as finishInput (the rewrite variant).
        while (runs_.size() > finalFrames_ - 1)
        {
            mergePass(mergeFrames_ - 1);
            ++stats_.passes;
        }
    }

    void ExternalSorter::startFinalMerge()
    {
        // An empty input has no run and no final pass.
        startMerge(0, runs_.size());
        if (!runs_.empty())
            ++stats_.passes;
    }

    bool ExternalSorter::next(Tuple& out)
    {
        if (!inMemory_)
            return nextMerged(out);
        if (buffer_.size() == 0)
            return false;
        // Copy, not move: popBack measures the tuple it removes.
        out = buffer_.tuples().back();
        buffer_.popBack();
        return true;
    }

    void ExternalSorter::mergePass(std::size_t fanIn)
    {
        // Frames: one reader page per run of the group + one output page
        // = fanIn + 1 = mergeFrames.
        std::vector<storage::TempFile> merged;
        for (std::size_t first = 0; first < runs_.size(); first += fanIn)
        {
            // Runs first .. last-1 become one run. A group of one run (left
            // over at the end) is copied too: the rewrite variant (D6).
            const std::size_t last = std::min(first + fanIn, runs_.size());
            startMerge(first, last);
            RunWriter writer(engine_, schema_);
            Tuple t;
            while (nextMerged(t))
                writer.append(t);
            merged.push_back(writer.finish());

            // Every reader is at its end and holds no page. Remove them, then
            // delete the group's runs at once: they are no longer needed.
            readers_.clear();
            for (std::size_t i = first; i < last; ++i)
                runs_[i].drop();
        }
        runs_ = std::move(merged);
    }

    void ExternalSorter::startMerge(std::size_t first, std::size_t last)
    {
        readers_.clear();
        heads_.clear();
        hasHead_.clear();
        for (std::size_t i = first; i < last; ++i)
        {
            readers_.push_back(std::make_unique<RunReader>(engine_, runs_[i], schema_));
            heads_.emplace_back();
            hasHead_.push_back(readers_.back()->next(heads_.back()));
        }
    }

    bool ExternalSorter::nextMerged(Tuple& out)
    {
        // Look at every reader's head tuple and take the smallest. Only a
        // strictly smaller head replaces the best so far, so on equal keys
        // the earlier run wins: equal tuples come out in the same order on
        // every run. With F runs this is F - 1 comparisons per tuple.
        const std::size_t none = readers_.size();
        std::size_t best = none;
        for (std::size_t i = 0; i < readers_.size(); ++i)
        {
            if (!hasHead_[i])
                continue; // this run is used up
            if (best == none || compareTuples(heads_[i], heads_[best]) < 0)
                best = i;
        }
        if (best == none)
            return false; // every run is used up

        out = std::move(heads_[best]);
        hasHead_[best] = readers_[best]->next(heads_[best]);
        return true;
    }
} // namespace db::exec
