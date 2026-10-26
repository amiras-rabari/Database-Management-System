#pragma once

// Replacement policies for the buffer pool: LRU and CLOCK (§M1.6).
//
// When the pool is full and needs room for a new page, a replacer answers
// one question: "which frame should I empty?" It never sees pages, files or
// the disk. The pool simply tells it what happened to each frame (used again,
// loaded, emptied) and asks it for a victim when needed.
//
// Free frames are handled by the pool, not here. While the pool still has
// frames that were never used, it fills them in order (0, 1, 2, ...) and
// only calls recordLoad. It asks chooseVictim only when every frame holds a
// page. That is why the first B misses on an empty pool are not evictions.
//
// This header is private to the storage module ("buffer/replacer.h"). Other
// modules choose a policy only through ReplacementPolicy.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "db/storage/replacement_policy.h"

namespace db::storage
{
    /// The number of a frame in the buffer pool: 0, 1, ..., B-1.
    /// It only lives in memory (never written to disk), so size_t is fine.
    using FrameId = std::size_t;

    /// What every replacement policy must offer. The pool calls these four
    /// functions without knowing whether LRU or CLOCK is behind them.
    /// Both policies update on every fetch, hit or miss (§M1.6).
    /// Passing a frame number that does not exist (>= B) is a bug in the
    /// caller and throws std::logic_error.
    class Replacer
    {
    public:
        /// Virtual, so that when the pool deletes its replacer through a
        /// Replacer pointer, the LRU or CLOCK destructor really runs.
        /// Without it, that delete would be undefined behaviour.
        virtual ~Replacer() = default;

        /// The page the pool was asked for was already in `frame` (a hit).
        virtual void recordHit(FrameId frame) = 0;

        /// A page was just put into `frame` (a miss, or a brand-new page).
        /// The frame was either a free one or the one chooseVictim picked.
        virtual void recordLoad(FrameId frame) = 0;

        /// Picks the frame to empty. Only frames with pin count 0 (nobody
        /// is using them) may be picked. Returns nothing if every frame is
        /// pinned. `pinCounts[i]` is frame i's pin count. The replacer may
        /// read the list but not change it: the pool owns the pin counts.
        /// The list must have one entry per frame, else std::logic_error.
        virtual std::optional<FrameId> chooseVictim(std::span<const std::uint32_t> pinCounts) = 0;

        /// `frame` is now empty: its page was thrown away (for example, a
        /// temporary file was deleted). The pool also puts the frame back
        /// on its free list, so chooseVictim is never asked about an empty
        /// frame, and reusing it does not count as an eviction.
        /// LRU will never pick a forgotten frame. CLOCK just sets its
        /// reference bit to 0, exactly as the spec's pseudocode allows.
        virtual void forget(FrameId frame) = 0;
    };

    /// LRU, "least recently used" (§M1.6): "Every fetch, hit or miss, moves
    /// that frame to most recently used. Victim = least recently used frame
    /// whose pin count is zero."
    ///
    /// How we track "recently": a counter that goes up by one every time any
    /// frame is used, like a ticket machine. The used frame keeps the new
    /// ticket number (its "stamp"). The smallest stamp was used longest ago,
    /// so it is the victim. Stamps start at 1; stamp 0 means "this frame
    /// holds no page" and is never picked.
    /// Picking a victim looks at every frame once (fine for B up to 256).
    /// No clock time is used, so every run gives the same answers.
    class LruReplacer : public Replacer
    {
    public:
        /// An LRU replacer for `frames` frames. 0 frames throws std::logic_error.
        explicit LruReplacer(std::size_t frames);

        void recordHit(FrameId frame) override;
        void recordLoad(FrameId frame) override;
        std::optional<FrameId> chooseVictim(std::span<const std::uint32_t> pinCounts) override;
        void forget(FrameId frame) override;

    private:
        /// lastUse_[f] is frame f's stamp: when it was last used. 0 = empty frame.
        std::vector<std::uint64_t> lastUse_;
        /// The last stamp handed out. It only goes up, so two frames never
        /// have the same stamp and there is never a tie to break.
        std::uint64_t counter_ = 0;
    };

    /// CLOCK (§M1.6): a cheaper version of LRU. Think of the frames placed
    /// in a circle like a clock face, with a hand pointing at one of them.
    /// Each frame has one "reference bit": 1 = used recently, 0 = not.
    /// "Every fetch, hit or miss, sets that frame's reference bit to 1. A
    /// newly loaded page gets ref bit 1 and the hand advances past it."
    /// chooseVictim follows the spec's pseudocode line by line, including
    /// the "swept > 2 * B" rule that stops the search when every frame is pinned.
    class ClockReplacer : public Replacer
    {
    public:
        /// A CLOCK replacer for `frames` frames. 0 frames throws std::logic_error.
        /// All reference bits start at 0 and the hand starts at frame 0.
        explicit ClockReplacer(std::size_t frames);

        void recordHit(FrameId frame) override;
        void recordLoad(FrameId frame) override;
        std::optional<FrameId> chooseVictim(std::span<const std::uint32_t> pinCounts) override;
        void forget(FrameId frame) override;

    private:
        /// ref_[f] is frame f's reference bit, 0 or 1. Stored as uint8_t, not
        /// bool, because std::vector<bool> is a special packed type that
        /// behaves differently from a normal vector.
        std::vector<std::uint8_t> ref_;
        /// The frame the hand points at: where the next search starts.
        /// Always between 0 and B-1.
        FrameId hand_ = 0;
    };

    /// Builds the replacer for `policy` with `frames` frames. The pool keeps
    /// it in the returned unique_ptr, which deletes it automatically, once.
    std::unique_ptr<Replacer> makeReplacer(ReplacementPolicy policy, std::size_t frames);
} // namespace db::storage
