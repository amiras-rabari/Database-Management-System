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

    LruReplacer::LruReplacer(std::size_t frames)
    {
        if (frames == 0)
            throw std::logic_error("LruReplacer: needs at least one frame");
        // One stamp per frame, all 0, because no frame holds a page yet.
        lastUse_.assign(frames, 0);
    }

    void LruReplacer::recordHit(FrameId frame)
    {
        requireFrame(frame, lastUse_.size(), "LruReplacer::recordHit");
        // Give the frame the next ticket: it is now the most recently used.
        lastUse_[frame] = ++counter_;
    }

    void LruReplacer::recordLoad(FrameId frame)
    {
        requireFrame(frame, lastUse_.size(), "LruReplacer::recordLoad");
        // Exactly the same as a hit. The spec says "every fetch, hit or
        // miss" makes the frame the most recently used (§M1.6).
        lastUse_[frame] = ++counter_;
    }

    /*
    What "pinned" means

    Imagine you are reading a page on a desk and someone takes it away in the
    middle of a sentence. Your code would suddenly read a different page's
    bytes. So whoever is using a page puts an "in use, don't touch" sticker
    on it:
    - Pin = add a sticker ("I'm using this").
    - Unpin = remove your sticker ("I'm done").
    - Pin count = how many stickers the frame has right now. Two operators
      reading the same page give a pin count of 2.

    The rule: a frame with pin count > 0 can never be the victim. Only frames
    with pin count 0 (nobody is using them) can be emptied.

    pinCounts is just the list of those numbers, one per frame:
        pinCounts = [1, 0, 0]
                     ^  ^  ^
                frame 0  frame 1  frame 2
    This reads: "frame 0 is in use, frames 1 and 2 may be emptied". The pool
    owns the stickers. It hands this list to the policy and says "pick a
    victim, but only among the zeros". The policy can read the list but not
    change it (that is what the `const` in span<const uint32_t> means).

    LRU: "Least Recently Used"

    Idea: empty the frame whose page nobody has touched for the longest time.
    If it was not needed lately, it probably will not be needed soon.

    How: every time a frame is used (hit or load), it gets a ticket number
    from a counter: 1, 2, 3, 4, ... The smallest number was used longest ago.
    - lastUse_ = the ticket number on each frame (its "stamp").
    - counter_ = the last ticket handed out.
    - Ticket 0 = "this frame is empty".

    Victim = the frame with no sticker and the smallest ticket.
    Example (spec trace, fetch of page 4): stamps are frame 0 = 4, frame 1 = 2,
    frame 2 = 3, all unpinned, so frame 1 (page 2) is the victim.
    */
    std::optional<FrameId> LruReplacer::chooseVictim(std::span<const std::uint32_t> pinCounts)
    {
        // One pin count per frame, or the loop below would read past the list.
        if (pinCounts.size() != lastUse_.size())
            throw std::logic_error("LruReplacer::chooseVictim: " + std::to_string(pinCounts.size())
                + " pin counts for " + std::to_string(lastUse_.size()) + " frames");

        // Look at every frame and keep the best candidate seen so far.
        // Skip frames that are pinned (in use) or empty (stamp 0).
        // A smaller stamp means "used longer ago", so it wins.
        // No two frames share a stamp, so the order we look in cannot change the answer.
        std::optional<FrameId> victim;
        for (FrameId f = 0; f < lastUse_.size(); ++f)
        {
            if (pinCounts[f] > 0 || lastUse_[f] == 0)
                continue;
            if (!victim || lastUse_[f] < lastUse_[*victim])
                victim = f;
        }
        // Still empty here means every frame is pinned: no victim.
        return victim;
    }

    void LruReplacer::forget(FrameId frame)
    {
        requireFrame(frame, lastUse_.size(), "LruReplacer::forget");
        // Stamp 0 = empty frame, so chooseVictim will skip it.
        lastUse_[frame] = 0;
    }
} // namespace db::storage
