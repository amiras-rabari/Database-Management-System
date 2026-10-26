#pragma once

// Victim selection for the buffer pool (§M1.6 LRU and CLOCK). Private to the
// storage module: included as "buffer/replacer.h" by the pool and the tests,
// never by other modules.
//
// A replacer decides only WHICH FRAME to reuse. It knows nothing about pages,
// files or disks; the pool tells it what happened to each frame.
//
// Free frames are not the replacer's job: while the pool still has
// never-used frames it fills them in index order (0, 1, 2, ...) and calls
// recordLoad. It calls chooseVictim only when no free frame is left, so the
// first B misses on an empty pool are not evictions.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "db/storage/replacement_policy.h"

namespace db::storage
{
    /// Index of a frame in the buffer pool, 0 .. B-1. Memory only, never
    /// stored on disk, so size_t.
    using FrameId = std::size_t;

    /// The interface the pool calls, whichever policy was chosen. Every
    /// fetch, hit or miss, updates the policy (§M1.6 for both policies).
    /// A frame number >= the frame count is a caller bug: std::logic_error.
    class Replacer
    {
    public:
        /// Virtual so that deleting an LruReplacer or ClockReplacer through a
        /// Replacer pointer (the pool's unique_ptr) runs the right destructor.
        virtual ~Replacer() = default;

        /// A fetch found its page already resident in `frame` (a HIT).
        virtual void recordHit(FrameId frame) = 0;

        /// A page was just loaded (or created) in `frame` (a MISS), either
        /// into a free frame or into the frame chooseVictim returned.
        virtual void recordLoad(FrameId frame) = 0;

        /// The frame to reuse, chosen among frames whose pin count is 0, or
        /// an empty optional if every frame is pinned. `pinCounts[i]` is
        /// frame i's pin count; read-only, the pool owns pin state. Its size
        /// must equal the frame count (else std::logic_error).
        virtual std::optional<FrameId> chooseVictim(std::span<const std::uint32_t> pinCounts) = 0;

        /// `frame` became empty (its page was discarded, e.g. a temp file
        /// was dropped). The pool must also put the frame back on its free
        /// list: chooseVictim runs only when no free frame exists, so it
        /// never sees an empty frame (reusing one is not an eviction).
        /// LRU never picks a forgotten frame; CLOCK sets its ref bit to 0
        /// decided as per the spec pseudocode unchanged.
        virtual void forget(FrameId frame) = 0;
    };

    /// LRU (§M1.6): "Every fetch, hit or miss, moves that frame to most
    /// recently used. Victim = least recently used frame whose pin count is
    /// zero."
    ///
    /// Design: logical timestamps. A counter goes up by one on every
    /// recordHit/recordLoad and the frame stores the new value. The victim is
    /// the unpinned frame with the smallest stamp. Stamps start at 1, so 0
    /// means "holds no page" (never loaded, or forgotten) and is never chosen.
    /// O(B) per victim; deterministic (no clock time, no addresses).
    class LruReplacer : public Replacer
    {
    public:
        /// A replacer for `frames` frames (must be > 0, else std::logic_error).
        explicit LruReplacer(std::size_t frames);

        void recordHit(FrameId frame) override;
        void recordLoad(FrameId frame) override;
        std::optional<FrameId> chooseVictim(std::span<const std::uint32_t> pinCounts) override;
        void forget(FrameId frame) override;

    private:
        /// lastUse_[f]: stamp of frame f's last access; 0 = holds no page.
        std::vector<std::uint64_t> lastUse_;
        /// The last stamp handed out. Strictly increasing, so no two frames
        /// ever share a stamp and there are no ties to break.
        std::uint64_t counter_ = 0;
    };

    /// CLOCK (§M1.6): frames in a circle, a reference bit each, and a hand.
    /// "Every fetch, hit or miss, sets that frame's reference bit to 1. A
    /// newly loaded page gets ref bit 1 and the hand advances past it."
    /// chooseVictim follows the spec pseudocode exactly, including the
    /// "swept > 2 * B" stop that ends the search when every frame is pinned.
    class ClockReplacer : public Replacer
    {
    public:
        /// A replacer for `frames` frames (must be > 0, else std::logic_error).
        /// All reference bits start at 0, the hand at frame 0.
        explicit ClockReplacer(std::size_t frames);

        void recordHit(FrameId frame) override;
        void recordLoad(FrameId frame) override;
        std::optional<FrameId> chooseVictim(std::span<const std::uint32_t> pinCounts) override;
        void forget(FrameId frame) override;

    private:
        /// ref_[f]: frame f's reference bit, 0 or 1. uint8_t rather than
        /// bool: std::vector<bool> is a packed special case with surprising
        /// behaviour.
        std::vector<std::uint8_t> ref_;
        /// The frame the next sweep looks at first; always < ref_.size().
        FrameId hand_ = 0;
    };

    /// The replacer for `policy` with `frames` frames. The pool owns it
    /// through the returned unique_ptr (freed automatically, exactly once).
    std::unique_ptr<Replacer> makeReplacer(ReplacementPolicy policy, std::size_t frames);
} // namespace db::storage
