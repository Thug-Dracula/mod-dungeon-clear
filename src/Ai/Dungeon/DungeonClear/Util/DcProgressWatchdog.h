/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPROGRESSWATCHDOG_H
#define _PLAYERBOT_DCPROGRESSWATCHDOG_H

#include <cstdint>

// One reusable "am I making progress toward my objective; escalate after a
// budget" detector, replacing the per-movement-mode stuck counters that had
// accreted one at a time (route-glide posStuck, its byte-identical door-walk-in
// twin, the swim closing-distance timer — nav review F11). Each site had grown
// its own copy of the metric + threshold + reset; folding them onto one tested
// primitive gives them a single implementation and a single Reset so they can't
// drift or be forgotten by a reset — the same win DcApproachState brought to the
// latch/counter sprawl, and it makes the state struct the honest home the review
// noted it already was.
//
// Two progress signals, because the sites legitimately differ:
//
//   * Displacement — "did the bot physically move this tick". The route glide and
//     its door-walk-in twin watch a continuous escort spline grinding against
//     geometry, where the wedge shows up as ~0 displacement WHILE isMoving(). A
//     momentary isMoving()==false (a spline micro-stop) is not a wedge, so it
//     resets. Blind to a fully stalled bot by design — that case belongs to other
//     rungs (direct-pursuit / dead-end) — matching the original posStuck exactly.
//
//   * ClosingDistance — "am I getting nearer the target". The swim leg walks
//     toward its current point; progress is the straight-line distance dropping
//     below the closest seen so far. Works whether or not the bot is moving. The
//     wall-clock stale check stays in the caller (getMSTimeDiff is wrap-safe and
//     lives in the engine layer), reading lastProgressMs.
//
// Deliberately NOT covering pursuitFailTicks / doneNotEngagedTicks: those are
// MoveTo-refusal / dead-end tick counters guarding the silent-freeze escalations,
// and recasting them as closing-distance changes their give-up semantics on the
// module's worst-symptom freezes — a live-validated tuning, not this
// behavior-preserving extraction.
struct DcProgressWatchdog
{
    std::uint32_t stuckTicks     = 0;      // consecutive no-progress ticks
    float         bestDist       = -1.0f;  // closest approach to the active target (<0 = unset)
    std::uint32_t lastProgressMs = 0;      // wall-clock of the last progress (time-budget sites)

    void Reset() { *this = DcProgressWatchdog{}; }

    // Displacement tick. moving = bot->isMoving(); moved = distance travelled
    // since the previous sample. A no-progress tick is one that is moving yet
    // barely displaced (moved < minMove); any not-moving OR real-movement tick
    // clears the count. Returns the updated counter — compare it against the
    // caller's tick budget. (The caller passes moving=false on the first,
    // un-sampled tick so it reads as "no wedge yet", exactly as the sentinel
    // lastPos did before.)
    std::uint32_t TickDisplacement(bool moving, float moved, float minMove)
    {
        if (moving && moved < minMove)
            ++stuckTicks;
        else
            stuckTicks = 0;
        return stuckTicks;
    }

    // Closing-distance tick. distToTarget = current straight-line distance to the
    // objective point; nowMs = getMSTime(). Progress = the distance improved by
    // >= minClose versus the closest seen so far (bestDist), which also re-arms
    // bestDist and stamps lastProgressMs. The first sample (bestDist < 0) arms the
    // tracker and counts as progress. Returns true iff progress was made this
    // tick; the caller compares getMSTimeDiff(lastProgressMs, nowMs) to its time
    // budget to decide when to give up.
    bool TickClosing(float distToTarget, float minClose, std::uint32_t nowMs)
    {
        if (bestDist < 0.0f || distToTarget < bestDist - minClose)
        {
            bestDist = distToTarget;
            lastProgressMs = nowMs;
            return true;
        }
        return false;
    }
};

#endif
