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

    LruReplacer::LruReplacer(std::size_t frames)
    {
        if (frames == 0)
            throw std::logic_error("LruReplacer: needs at least one frame");
        // One stamp per frame, all 0: no frame holds a page yet.
        lastUse_.assign(frames, 0);
    }

    void LruReplacer::recordHit(FrameId frame)
    {
        requireFrame(frame, lastUse_.size(), "LruReplacer::recordHit");
        // The next stamp makes this frame the most recently used.
        lastUse_[frame] = ++counter_;
    }

    void LruReplacer::recordLoad(FrameId frame)
    {
        requireFrame(frame, lastUse_.size(), "LruReplacer::recordLoad");
        // Same as a hit: §M1.6 "every fetch, hit or miss" moves the frame to
        // most recently used.
        lastUse_[frame] = ++counter_;
    }

    /*What "pinned" means

    Imagine you're reading a page on the desk, and someone else takes it away mid-sentence.
    That would break everything:
    the code would suddenly be reading a different page's bytes.

    So whoever is using a page puts a "in use, don't touch" sticker on it:
    - Pin = add a sticker ("I'm using this").
    - Unpin = remove your sticker ("I'm done").
    - Pin count = how many stickers the frame has right now. Two operators reading the same page give
    a pin count of 2.

    The rule: a frame with pin count > 0 can never be the victim. Only frames with count 0
    (nobody using them) can be removed.

    pinCounts in the code is just a list of those numbers, one per frame:
    pinCounts = [1, 0, 0]
                 ^  ^  ^
            frame0 frame1 frame2
    This reads: "frame 0 is in use, frames 1 and 2 are free to evict". The buffer pool owns the stickers.
    It hands this list to the policy and says "pick a victim, but only among the zeros." The policy
    can read the list but can't change it (that's what const in span<const uint32_t> means).

    LRU: "Least Recently Used"

    Idea: throw away the page nobody has touched for the longest time.
    If you haven't needed it lately, you probably won't need it soon.

    How: every time a frame is used (hit or load), we write a ticket number on it
    from a counter: 1, 2, 3, 4, … The smallest number was used longest ago. The stamp is that ticket number.
    - lastUse_ = the ticket number on each frame.
    - counter_ = the next ticket to hand out.
    - Ticket 0 = "this frame is empty".

    Victim = the frame with no sticker and the smallest ticket.
    */
    std::optional<FrameId> LruReplacer::chooseVictim(std::span<const std::uint32_t> pinCounts)
    {
        if (pinCounts.size() != lastUse_.size())
            throw std::logic_error("LruReplacer::chooseVictim: " + std::to_string(pinCounts.size())
                + " pin counts for " + std::to_string(lastUse_.size()) + " frames");

        // Smallest stamp among unpinned frames that hold a page (stamp != 0).
        // Stamps are unique, so the scan order cannot change the answer.
        std::optional<FrameId> victim;
        for (FrameId f = 0; f < lastUse_.size(); ++f)
        {
            if (pinCounts[f] > 0 || lastUse_[f] == 0)
                continue;
            if (!victim || lastUse_[f] < lastUse_[*victim])
                victim = f;
        }
        return victim;
    }

    void LruReplacer::forget(FrameId frame)
    {
        requireFrame(frame, lastUse_.size(), "LruReplacer::forget");
        lastUse_[frame] = 0;
    }
} // namespace db::storage
