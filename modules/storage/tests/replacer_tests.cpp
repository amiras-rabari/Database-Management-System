// Replacement policies: the SPEC M1.6 trace `1 2 3 1 4 2` with B = 3 for LRU
// and CLOCK, pinned frames, forget, the CLOCK hand rule and policy names.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <doctest/doctest.h>

#include "buffer/replacer.h"
#include "db/storage/replacement_policy.h"

namespace
{

    using db::storage::ClockReplacer;
    using db::storage::FrameId;
    using db::storage::LruReplacer;
    using db::storage::makeReplacer;
    using db::storage::parsePolicy;
    using db::storage::policyName;
    using db::storage::Replacer;
    using db::storage::ReplacementPolicy;

    constexpr int kNoPage = -1;

    // What a trace run produced, counted the §M1.6 way.
    struct TraceResult
    {
        int hits = 0;
        int misses = 0;
        int evictions = 0;
        std::vector<int> evictedPages; // in eviction order
        std::vector<int> pageInFrame;  // frame contents at the end

        bool operator==(const TraceResult&) const = default;
    };

    // A tiny buffer pool: `frames` frames, every page unpinned right after
    // its fetch (all pin counts 0). Free frames are filled in index order
    // before the replacer is asked for a victim.
    TraceResult runTrace(Replacer& replacer, std::size_t frames, const std::vector<int>& trace)
    {
        TraceResult result;
        result.pageInFrame.assign(frames, kNoPage);
        const std::vector<std::uint32_t> pinCounts(frames, 0);

        for (int page : trace)
        {
            std::optional<FrameId> resident;
            for (FrameId f = 0; f < frames; ++f)
                if (result.pageInFrame[f] == page)
                    resident = f;
            if (resident)
            {
                ++result.hits;
                replacer.recordHit(*resident);
                continue;
            }

            ++result.misses;
            std::optional<FrameId> target;
            for (FrameId f = 0; f < frames && !target; ++f)
                if (result.pageInFrame[f] == kNoPage)
                    target = f;
            if (!target)
            {
                target = replacer.chooseVictim(pinCounts);
                REQUIRE(target.has_value());
                ++result.evictions;
                result.evictedPages.push_back(result.pageInFrame[*target]);
            }
            result.pageInFrame[*target] = page;
            replacer.recordLoad(*target);
        }
        return result;
    }

    const std::vector<int> kSpecTrace{1, 2, 3, 1, 4, 2};

} // namespace

TEST_CASE("LRU reproduces the SPEC M1.6 trace: 1 hit, 5 misses, 2 evictions")
{
    LruReplacer replacer(3);
    const TraceResult result = runTrace(replacer, 3, kSpecTrace);
    CHECK(result.hits == 1);
    CHECK(result.misses == 5);
    CHECK(result.evictions == 2);
    CHECK(result.evictedPages == std::vector<int>{2, 3});
    // Page 4 replaced page 2 in frame 1, page 2 replaced page 3 in frame 2.
    CHECK(result.pageInFrame == std::vector<int>{1, 4, 2});
}

TEST_CASE("CLOCK reproduces the SPEC M1.6 trace: 2 hits, 4 misses, 1 eviction")
{
    ClockReplacer replacer(3);
    const TraceResult result = runTrace(replacer, 3, kSpecTrace);
    CHECK(result.hits == 2);
    CHECK(result.misses == 4);
    CHECK(result.evictions == 1);
    CHECK(result.evictedPages == std::vector<int>{1});
    CHECK(result.pageInFrame == std::vector<int>{4, 2, 3});
}

TEST_CASE("makeReplacer builds the policy asked for")
{
    auto lru = makeReplacer(ReplacementPolicy::Lru, 3);
    REQUIRE(lru != nullptr);
    CHECK(runTrace(*lru, 3, kSpecTrace).evictions == 2);

    auto clock = makeReplacer(ReplacementPolicy::Clock, 3);
    REQUIRE(clock != nullptr);
    CHECK(runTrace(*clock, 3, kSpecTrace).evictions == 1);
}

TEST_CASE("the same trace twice gives identical results (determinism)")
{
    LruReplacer lruA(3);
    LruReplacer lruB(3);
    CHECK(runTrace(lruA, 3, kSpecTrace) == runTrace(lruB, 3, kSpecTrace));
    ClockReplacer clockA(3);
    ClockReplacer clockB(3);
    CHECK(runTrace(clockA, 3, kSpecTrace) == runTrace(clockB, 3, kSpecTrace));
}

TEST_CASE("every frame pinned: no victim, and CLOCK stops")
{
    const std::vector<std::uint32_t> allPinned{1, 1, 1};

    LruReplacer lru(3);
    ClockReplacer clock(3);
    for (FrameId f = 0; f < 3; ++f)
    {
        lru.recordLoad(f);
        clock.recordLoad(f);
    }
    CHECK_FALSE(lru.chooseVictim(allPinned).has_value());
    // Reaching the next line at all proves the sweep ends (swept > 2 * B).
    CHECK_FALSE(clock.chooseVictim(allPinned).has_value());
}

TEST_CASE("LRU skips a pinned frame even if it is least recently used")
{
    LruReplacer lru(3);
    for (FrameId f = 0; f < 3; ++f)
        lru.recordLoad(f);
    // Frame 0 is the oldest but pinned, so frame 1 is next.
    CHECK(lru.chooseVictim(std::vector<std::uint32_t>{1, 0, 0}) == std::optional<FrameId>(1));
    // A hit makes frame 1 the newest, so frame 2 is now the oldest unpinned.
    lru.recordHit(1);
    CHECK(lru.chooseVictim(std::vector<std::uint32_t>{1, 0, 0}) == std::optional<FrameId>(2));
}

TEST_CASE("CLOCK passes over a pinned frame without clearing its reference bit")
{
    ClockReplacer clock(3);
    for (FrameId f = 0; f < 3; ++f)
        clock.recordLoad(f); // refs 1,1,1; hand back at frame 0

    // Frame 0 pinned: skip 0, clear 1, clear 2, skip 0, frame 1 has ref 0.
    CHECK(clock.chooseVictim(std::vector<std::uint32_t>{1, 0, 0}) == std::optional<FrameId>(1));
    // Hand at 2, whose ref is 0.
    CHECK(clock.chooseVictim(std::vector<std::uint32_t>{0, 0, 0}) == std::optional<FrameId>(2));
    // Hand at 0. Frame 0's bit must still be 1 (pinned frames are only
    // passed over), so it is cleared and frame 1 (ref 0) is the victim.
    // A sweep that had cleared frame 0's bit while it was pinned picks 0.
    CHECK(clock.chooseVictim(std::vector<std::uint32_t>{0, 0, 0}) == std::optional<FrameId>(1));
}

TEST_CASE("CLOCK moves the hand past a frame loaded into a free slot")
{
    // The classic bug: recordLoad sets ref = 1 but leaves the hand alone.
    ClockReplacer clock(3);
    for (FrameId f = 0; f < 3; ++f)
        clock.recordLoad(f); // hand back at 0
    clock.forget(1);         // frame 1 emptied ...
    clock.recordLoad(1);     // ... and refilled: the hand must move to 2
    // All refs are 1, so a full sweep clears them and the victim is the
    // frame where the sweep started: 2 if the hand moved, 0 if it did not.
    CHECK(clock.chooseVictim(std::vector<std::uint32_t>{0, 0, 0}) == std::optional<FrameId>(2));
}

TEST_CASE("LRU never returns a forgotten frame")
{
    LruReplacer lru(3);
    for (FrameId f = 0; f < 3; ++f)
        lru.recordLoad(f);
    lru.forget(0); // the oldest frame now holds no page
    CHECK(lru.chooseVictim(std::vector<std::uint32_t>{0, 0, 0}) == std::optional<FrameId>(1));
}

TEST_CASE("misuse is a logic_error")
{
    CHECK_THROWS_AS(LruReplacer(0), std::logic_error);
    CHECK_THROWS_AS(ClockReplacer(0), std::logic_error);

    LruReplacer lru(3);
    ClockReplacer clock(3);
    CHECK_THROWS_AS(lru.recordHit(3), std::logic_error);
    CHECK_THROWS_AS(clock.recordLoad(3), std::logic_error);
    CHECK_THROWS_AS(lru.forget(7), std::logic_error);
    CHECK_THROWS_AS(lru.chooseVictim(std::vector<std::uint32_t>{0, 0}), std::logic_error);
    CHECK_THROWS_AS(clock.chooseVictim(std::vector<std::uint32_t>{0, 0, 0, 0}), std::logic_error);
}

TEST_CASE("policy names round-trip")
{
    CHECK(parsePolicy("lru") == std::optional<ReplacementPolicy>(ReplacementPolicy::Lru));
    CHECK(parsePolicy("clock") == std::optional<ReplacementPolicy>(ReplacementPolicy::Clock));
    CHECK_FALSE(parsePolicy("fifo").has_value());
    CHECK_FALSE(parsePolicy("LRU").has_value());
    CHECK_FALSE(parsePolicy("").has_value());
    CHECK(policyName(ReplacementPolicy::Lru) == "lru");
    CHECK(policyName(ReplacementPolicy::Clock) == "clock");
    CHECK(parsePolicy(policyName(ReplacementPolicy::Clock)) == std::optional<ReplacementPolicy>(ReplacementPolicy::Clock));
}
