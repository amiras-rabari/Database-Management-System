#include "buffer/replacer.h"

#include <stdexcept>
#include <string>

namespace db::storage
{
    namespace
    {
        // A frame number outside 0 .. B-1 is a caller bug (D1).
        void requireFrame(FrameId frame, std::size_t frames, const char* where)
        {
            if (frame >= frames)
                throw std::logic_error(std::string(where) + ": frame " + std::to_string(frame)
                    + " out of range (" + std::to_string(frames) + " frames)");
        }
    } // namespace

    ClockReplacer::ClockReplacer(std::size_t frames)
    {
        if (frames == 0)
            throw std::logic_error("ClockReplacer: needs at least one frame");
        // One reference bit per frame, all 0; hand_ starts at frame 0.
        ref_.assign(frames, 0);
    }

    void ClockReplacer::recordHit(FrameId frame)
    {
        requireFrame(frame, ref_.size(), "ClockReplacer::recordHit");
        // A hit sets the bit; the hand does not move.
        ref_[frame] = 1;
    }

    void ClockReplacer::recordLoad(FrameId frame)
    {
        requireFrame(frame, ref_.size(), "ClockReplacer::recordLoad");
        // §M1.6 "A newly loaded page gets ref bit 1 and the hand advances
        // past it". After chooseVictim the hand is already there; after the
        // pool filled a free frame this moves it.
        ref_[frame] = 1;
        hand_ = (frame + 1) % ref_.size();
    }

    /*
    * LRU has to remember exact order. CLOCK only remembers one yes/no per frame: "used recently?"
    - ref_ (the "reference bit") is that yes/no: 1 = used recently, 0 = not.
    - Imagine the frames arranged in a circle like a clock face, with a hand pointing at one.
    hand_ = which frame it points at.

    To pick a victim, the hand walks around the circle:
    - Frame has a sticker (pinned)? Skip it.
    - Frame has ref = 1? Say "you get a second chance", set it to 0, and move on.
    - Frame has ref = 0 and no sticker? That's the victim.

    If the hand goes around twice and finds nothing, every frame must have a sticker.
    The answer is "no victim", and the pool reports an error. That's the swept > 2·B rule:
    it stops the hand walking forever.
     */
    std::optional<FrameId> ClockReplacer::chooseVictim(std::span<const std::uint32_t> pinCounts)
    {
        const std::size_t frames = ref_.size();
        if (pinCounts.size() != frames)
            throw std::logic_error("ClockReplacer::chooseVictim: " + std::to_string(pinCounts.size())
                + " pin counts for " + std::to_string(frames) + " frames");

        // The §M1.6 pseudocode, step for step. `hand += 1` wraps: after
        // frame B-1 comes frame 0.
        std::size_t swept = 0;
        while (true)
        {
            const FrameId f = hand_;
            if (pinCounts[f] > 0)
            {
                // Pinned: passed over, its reference bit is left alone.
                hand_ = (hand_ + 1) % frames;
                ++swept;
            }
            else if (ref_[f] == 1)
            {
                // Second chance: clear the bit and move on.
                ref_[f] = 0;
                hand_ = (hand_ + 1) % frames;
                ++swept;
            }
            else
            {
                hand_ = (hand_ + 1) % frames;
                return f;
            }
            // One turn clears every bit; a second turn with no victim means
            // every frame is pinned.
            if (swept > 2 * frames)
                return std::nullopt;
        }
    }

    void ClockReplacer::forget(FrameId frame)
    {
        requireFrame(frame, ref_.size(), "ClockReplacer::forget");
        ref_[frame] = 0;
    }

} // namespace db::storage
