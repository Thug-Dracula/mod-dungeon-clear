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
    float       returnLegStartDist = 0.0f;       // bot->camp distance stamped when
                                                 // the drag-back (Returning) begins;
                                                 // the turn-and-plant gate requires
                                                 // at least half of it covered before
                                                 // the tank may stop and fight. 0 =
                                                 // no leg in progress.
    uint32      plantTicks = 0;                   // consecutive maneuver ticks the
                                                 // pack has been gathered at the
                                                 // tank during the drag-back; the
                                                 // debounce latch for
                                                 // DungeonClearMath::ShouldPlantEarly.
    bool        losPull    = false;              // this pull targets a RANGED pack
                                                 // and the camp was placed to break
                                                 // line of sight to it (rangers must
                                                 // close to melee). Stamped at commit
                                                 // for the addon status line.
    uint32      campPublishedMs = 0;             // getMSTime() of the last camp
                                                 // write by the PULL machinery
                                                 // (prospective publish, commit,
                                                 // dynamic-upgrade seed, fresh
                                                 // unplanned-aggro camp). Advance's
                                                 // scout camp-trailing runs only
                                                 // while this is STALE (see
                                                 // DC_CAMP_PUBLISH_FRESH_MS), so
                                                 // exactly one owner moves the camp
                                                 // on any tick and it can never
                                                 // silently freeze between them.

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

    // --- engage-fizzle latch ------------------------------------------------
    ObjectGuid  pullTarget;                      // pack THIS pull tagged (stamped at
                                                 // commit and on unplanned scout
                                                 // aggro; NOT cleared by EnterEngage).
                                                 // Read at the Engage cleanup to
                                                 // detect a fizzled drag: the target
                                                 // still alive and idle after the
                                                 // "camp fight" means it never came.
    ObjectGuid  fizzleTarget;                    // pack whose pulls keep fizzling
    uint32      fizzleCount = 0;                 // consecutive fizzles of fizzleTarget;
                                                 // at DC_PULL_FIZZLE_MAX the pack is
                                                 // handed to the walk-in engage via
                                                 // abortTarget instead of re-pulled

    // --- dynamic verdict (pull setting == 2) ------------------------------
    uint32      decision       = 0;              // 0 none / 1 leeroy / 2 advanced
    ObjectGuid  decisionTarget;                  // pack the verdict applies to
    uint32      decisionSince  = 0;              // last re-classification timestamp
    uint32      patrolWaitSince = 0;             // getMSTime() the current pull first
                                                 // read patrol-contended (a lone
                                                 // patroller is the only thing over
                                                 // the Leeroy ceiling) while the tank
                                                 // holds at commit range; 0 = not
                                                 // waiting. Timeout latch for
                                                 // DungeonClearMath::ShouldWaitForPatrol
                                                 // (decision == 3 surfaces it). Reset
                                                 // when the pack changes.
    uint32      targetLostSince = 0;             // getMSTime() the pull target first
                                                 // resolved null while a Dynamic
                                                 // verdict was standing; 0 = present.
                                                 // Grace latch for DungeonClearMath::
                                                 // ShouldDropPullVerdict.

    void Reset() { *this = DcPullContext{}; }

    // The ONLY way to transition into Engage (used by DcSetPullPhase and
    // DcLeaderSignal::AbortLeaderPull, which live in different TUs). Entering
    // Engage ends the pull — camp arrival, CC-abort, evade release, return-leg
    // wedge, or a forced abort — and the tag latch is per-PULL, not per-run: the
    // next pull of the same still-alive pack must be free to tag it again.
    // Without the clear, a re-pull of an evaded pack sits in "already tagged,
    // hold for aggro" until the leg watchdog dumps it to the normal walk-in.
    // abortTarget is deliberately NOT touched here: its lifecycle (set on tag
    // timeout/wedge, cleared at the Engage->Idle cleanup) prevents pull livelock
    // on a problem pack.
    void EnterEngage(uint32 nowMs)
    {
        phase = DcPullPhase::Engage;
        phaseSince = nowMs;
        tagTarget = ObjectGuid::Empty;
    }
};

#endif
