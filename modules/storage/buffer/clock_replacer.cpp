#include "buffer/replacer.h"

#include <stdexcept>
#include <string>

namespace db::storage
{
    namespace
    {
        // Throws if `frame` is not one of the B frames (0 .. B-1).
        // That can only happen through a bug in the caller (D1).
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
        // One reference bit per frame, all 0. The hand starts at frame 0.
        ref_.assign(frames, 0);
    }

    void ClockReplacer::recordHit(FrameId frame)
    {
        requireFrame(frame, ref_.size(), "ClockReplacer::recordHit");
        // A hit marks the frame "used recently". The hand stays where it is.
        ref_[frame] = 1;
    }

    void ClockReplacer::recordLoad(FrameId frame)
    {
        requireFrame(frame, ref_.size(), "ClockReplacer::recordLoad");
        // §M1.6: "A newly loaded page gets ref bit 1 and the hand advances
        // past it." So the hand moves to the next frame (wrapping after B-1).
        // If the frame came from chooseVictim, the hand is already there.
        // If the pool filled a free frame, this is what moves the hand.
        // Example: after loading frames 0, 1, 2 (B = 3) the hand is back at 0.
        ref_[frame] = 1;
        hand_ = (frame + 1) % ref_.size();
    }

    /*
    LRU has to remember the exact order of use. CLOCK only remembers one
    yes/no per frame: "used recently?"
    - ref_ (the "reference bit") is that yes/no: 1 = used recently, 0 = not.
    - Imagine the frames placed in a circle like a clock face, with a hand
      pointing at one of them. hand_ = the frame it points at.

    To pick a victim, the hand walks around the circle:
    - Frame has a sticker (pinned)? Skip it.
    - Frame has ref = 1? Give it a "second chance": set it to 0 and move on.
    - Frame has ref = 0 and no sticker? That is the victim.

    If the hand goes around twice and finds nothing, every frame must have a
    sticker. The answer is "no victim", and the pool reports an error. That
    is the swept > 2 * B rule: it stops the hand from walking forever.

    Example (spec trace, fetch of page 4, B = 3): frames hold pages 1, 2, 3,
    all with ref 1, hand at frame 0. The hand clears frames 0, 1, 2, comes
    back to frame 0 (now ref 0) and picks it: page 1 is the victim.
    */
    std::optional<FrameId> ClockReplacer::chooseVictim(std::span<const std::uint32_t> pinCounts)
    {
        const std::size_t frames = ref_.size();
        // One pin count per frame, or the loop below would read past the list.
        if (pinCounts.size() != frames)
            throw std::logic_error("ClockReplacer::chooseVictim: " + std::to_string(pinCounts.size())
                + " pin counts for " + std::to_string(frames) + " frames");

        // The §M1.6 pseudocode, step by step.
        // `(hand_ + 1) % frames` moves the hand one frame on; after the
        // last frame it wraps back to frame 0.
        // `swept` counts frames the hand passed without picking one.
        std::size_t swept = 0;
        while (true)
        {
            const FrameId f = hand_;
            if (pinCounts[f] > 0)
            {
                // In use: skip it. Its reference bit is left as it is.
                hand_ = (hand_ + 1) % frames;
                ++swept;
            }
            else if (ref_[f] == 1)
            {
                // Used recently: second chance. Clear the bit and move on.
                ref_[f] = 0;
                hand_ = (hand_ + 1) % frames;
                ++swept;
            }
            else
            {
                // Not pinned and not used since the hand last came by:
                // this is the victim. The hand still moves past it, so the
                // next search starts at the frame after it.
                hand_ = (hand_ + 1) % frames;
                return f;
            }
            // The first full turn clears every bit. If a second full turn
            // still finds no victim, every frame is pinned: give up.
            if (swept > 2 * frames)
                return std::nullopt;
        }
    }

    void ClockReplacer::forget(FrameId frame)
    {
        requireFrame(frame, ref_.size(), "ClockReplacer::forget");
        // The frame is empty now, so it was not "used recently".
        ref_[frame] = 0;
    }

} // namespace db::storage
