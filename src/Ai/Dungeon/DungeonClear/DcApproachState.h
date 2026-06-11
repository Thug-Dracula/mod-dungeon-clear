/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCAPPROACHSTATE_H
#define _PLAYERBOT_DCAPPROACHSTATE_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

// All transient per-approach state for one boss-approach run, owned as a single
// value (DungeonClearApproachStateValue, "dungeon clear approach state") so the
// whole advance state machine resets in lockstep through exactly one Reset().
// This replaced nine loose ManualSetValue globals — the position-stuck and
// MoveTo-noop counters, the consecutive-rebuild count, the direct-pursuit and
// path-ends-short give-up latches, the loot-yield commit anchor, the position
// sentinel + last committed boss entry, and the long-path cache state — whose
// resets were scattered across two free functions in the action TU and ~25
// inline ->Set(0u) calls in the chat actions, kept in sync by hand. That was
// the identical failure mode the pull FSM fixed: "the approach is flaky" almost
// always meant "one of N globals didn't reset in lockstep" (a stale latch
// surviving pause / skip / resume / boss change).
//
// Add a new per-approach field HERE (never as a separate value) so it can never
// be forgotten by a reset. Mirrors DcPullContext. The two named subset resets
// (OnBossChange / OnEnteredEngageRange) replace the old ResetApproachOn* free
// functions so the reset subsets are named, visible, and co-located instead of
// inline ->Set(0u) clusters.
struct DcApproachState
{
    // --- per-approach stuck / recovery counters ---------------------------
    uint32 posStuckTicks       = 0;  // no-displacement ticks (was "stuck ticks")
    uint32 stuckCount          = 0;  // MoveTo-returned-false backup (was "stuck count")
    uint32 rebuildAttempts     = 0;  // consecutive rebuilds w/o progress ("stride rebuild attempts")
    uint32 pursuitFailTicks    = 0;  // direct-pursuit give-up latch ("pursuit fail ticks")
    uint32 doneNotEngagedTicks = 0;  // path-ends-short escalation ("done-not-engaged ticks")

    // --- approach bookkeeping ---------------------------------------------
    Position lastPos;                // previous-tick world pos; (0,0,0) = not yet sampled
    uint32 lastTargetEntry     = 0;  // committed boss entry the approach is for
    uint32 lootYieldStartMs    = 0;  // loot-yield commit anchor (getMSTime)

    // --- blocking-door interaction ----------------------------------------
    // Last door the bot clicked open and when, so the door-blocked action can
    // re-click an auto-closing gate (Strat's King's Square Gate re-shuts 3s
    // after opening) on a cooldown instead of the old one-shot announce latch,
    // which deadlocked: if the gate re-closed before Advance ran and cleared
    // the latch, the bot never clicked again and sat "Blocked" forever.
    ObjectGuid lastDoorUseGuid;      // door the last Use() was issued on
    uint32 lastDoorUseMs       = 0;  // when it was issued (getMSTime)

    // Blocked-state watchdog: how long the door-blocked action has been parked
    // at one closed door it believes it can open, without the door actually
    // opening. The entitlement check is template-level and CAN be wrong (SFK's
    // Arugal's Lair is an event door wearing the same empty-lock-85 template as
    // a plain clickable Deadmines door), and the click gate measures range to
    // the GO origin, which on wide gates sits outside DC_DOOR_USE_RANGE of the
    // path-side parking spot — both leave the bot "working" a door forever.
    // After DungeonClear.DoorBlockedTimeout seconds the action gives up and
    // takes the same auto-pause path as a door it knows it can't open.
    // doorStallLastMs re-arms the window: a gap in observations means the stall
    // ended (door opened, run moved on) and a later stall starts fresh.
    ObjectGuid doorStallGuid;        // door the current Blocked stall is on
    uint32 doorStallSinceMs    = 0;  // when that stall began (getMSTime)
    uint32 doorStallLastMs     = 0;  // last tick the stall was observed

    // --- long-path cache state --------------------------------------------
    // The cached long-range A* result lives in its own value ("dungeon clear
    // long path"); these are the bookkeeping fields that govern when it rebuilds
    // and what it was built toward. longPathTargetEntry doubles as the cache key
    // (read by DcStatusPublisher for the route-target display).
    uint32 longPathTargetEntry = 0;  // boss entry the cache was built for (cache key)
    Position longPathTargetPos;      // world pos the cache was built toward (retarget check)
    uint32 longPathExpiresMs   = 0;  // cache TTL deadline (getMSTime); 0 = force rebuild
    uint64 pendingPathJob      = 0;  // in-flight async build job id (0 = none)
    uint32 pendingPathSinceMs  = 0;  // when the pending job was submitted (watchdog)

    // Full reset: every approach AND long-path-cache field. Used on dc on/off,
    // death, all-cleared, and every pull interrupt — the run-state teardown.
    void Reset() { *this = DcApproachState{}; }

    // Committed boss changed: wipe every per-approach counter/latch and the
    // position sentinel so nothing from the previous pull bleeds into the new
    // approach. Leaves the long-path cache fields alone — EnsureLongPath manages
    // those off the new entry. (The sticky engage-trash target is a separate
    // value reset at the call site, not part of this struct.)
    void OnBossChange(uint32 newEntry)
    {
        lastTargetEntry     = newEntry;
        stuckCount          = 0;
        posStuckTicks       = 0;
        rebuildAttempts     = 0;
        doneNotEngagedTicks = 0;
        pursuitFailTicks    = 0;
        lastPos             = Position();
        doorStallGuid.Clear();
        doorStallSinceMs    = 0;
        doorStallLastMs     = 0;
    }

    // Reached engage range: clear the dead-end escalation counter and the
    // direct-pursuit give-up latch, so a boss that wanders back out can be
    // re-pursued cleanly instead of staying latched off the pursuit shortcut.
    void OnEnteredEngageRange()
    {
        doneNotEngagedTicks = 0;
        pursuitFailTicks    = 0;
    }
};

#endif
