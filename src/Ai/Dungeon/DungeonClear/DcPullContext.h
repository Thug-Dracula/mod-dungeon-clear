/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPULLCONTEXT_H
#define _PLAYERBOT_DCPULLCONTEXT_H

#include <vector>

#include "ObjectGuid.h"
#include "Position.h"

// Advanced-pull (LOS pull-to-camp) sub-phase. The leader's private control state
// AND the cross-context signal followers read to decide when to hold + go passive
// at camp. Forming/Advancing/Returning are the "holding" phases that keep the
// party passive at camp; Idle and Engage release them. See
// DcLeaderSignal::IsPullPhaseHolding.
enum class DcPullPhase : uint32
{
    Idle      = 0,
    Forming   = 1,
    Advancing = 2,
    Returning = 3,
    Engage    = 4,
};

// All transient state for one advanced/dynamic pull run, owned as a single value
// (DungeonClearPullContextValue, "dungeon clear pull context") so the whole pull
// FSM resets in lockstep through exactly one Reset(). This replaced seven loose
// ManualSetValue globals whose resets were scattered and provably incomplete —
// the "stale latch survives pause/skip/resume" bug class. Add a new pull-run
// field HERE (never as a separate value) so it can never be forgotten by a reset.
//
// Leader-owned. Followers read `phase`/`camp` cross-context through the leader's
// copy of this value (DcLeaderSignal::GetLeaderPullInfo / GetLeaderCampHold).
struct DcPullContext
{
    // --- FSM sequencing ---------------------------------------------------
    DcPullPhase phase      = DcPullPhase::Idle;  // current sub-phase
    uint32      phaseSince = 0;                  // getMSTime() at last transition
                                                 // (Forming dwell + per-leg watchdogs)

    // --- maneuver geometry ------------------------------------------------
    Position    camp;                            // party hold / drag-back point;
                                                 // (0,0,0) = unset
    std::vector<Position> breadcrumbs;           // recently-walked trail used to
                                                 // place the camp behind the tank

    // --- CC-assist gate ---------------------------------------------------
    uint32      ccSince    = 0;                  // getMSTime() the tank's CURRENT
                                                 // continuous drag-ruining CC
                                                 // (stun/fear/confuse/root/heavy
                                                 // slow) began; 0 = not CC'd. The
                                                 // grace latch for "tank CC'd mid
                                                 // drag -> abort pull, party piles
                                                 // in" (DungeonClearMath::Should
                                                 // AbortPullForCc).

    // --- per-target latches -----------------------------------------------
    ObjectGuid  abortTarget;                     // pack a pull gave up on; don't re-pull
    ObjectGuid  tagTarget;                        // "already tagged, hold for aggro" /
                                                  // pull-spell-opener dedupe (was
                                                  // "dungeon clear last pull target")

    // --- dynamic verdict (pull setting == 2) ------------------------------
    uint32      decision       = 0;              // 0 none / 1 leeroy / 2 advanced
    ObjectGuid  decisionTarget;                  // pack the verdict applies to
    uint32      decisionSince  = 0;              // last re-classification timestamp

    void Reset() { *this = DcPullContext{}; }
};

#endif
