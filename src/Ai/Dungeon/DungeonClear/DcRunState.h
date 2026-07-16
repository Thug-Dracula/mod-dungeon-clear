/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRUNSTATE_H
#define _PLAYERBOT_DCRUNSTATE_H

#include <string>

#include "ObjectGuid.h"

// The authoritative, leader-owned state of one dungeon-clear RUN — the run's
// identity/mode, its current manual-override objective, and the two cross-bot
// leader-fight signals that used to live in translation-unit-static maps. Owned
// as a single value (DungeonClearRunStateValue, "dungeon clear run state") so the
// whole run resets in lockstep through named transitions, exactly like the
// sub-feature structs DcApproachState and DcPullContext already do one level down.
//
// This is the third and last of the "one struct, one Reset()" consolidations that
// each ended a recurring "the X is flaky" family. DcApproachState fixed the
// approach FSM; DcPullContext fixed the pull FSM; DcRunState fixes the PARTY/RUN
// level — the enabled/paused/pause-cluster/selected-boss values whose resets were
// hand-replicated (as slightly different subsets) across DisableDungeonClear and
// the DcOn/Off/Skip/Go/Resume chat-action clusters, plus the leader-combat-since
// and party-engaged latches that were file-static maps each with their own mutex.
// A stale latch surviving a pause / skip / resume / boss-change was the single
// most common root cause in the whole bug log; folding these here so exactly one
// Reset() clears them makes that class structurally impossible.
//
// Add a new run-level field HERE (never as a separate value) so it can never be
// forgotten by a reset.
//
// Leader-owned. Followers read `enabled`/`paused` cross-context through the
// leader's copy of this value (DcLeaderSignal::IsInPausedDungeonClearRun and the
// party-tank / camp-hold gates), the same pattern DcPullContext uses. Each bot
// still holds its OWN DcRunState value; a follower's stays at defaults (it never
// leads a run) and reading it is harmless.
//
// NOTE on the pull PREFERENCE: the advanced-pull tri-state (`dungeon clear pull
// setting`) and its behavioral bool (`dungeon clear pull mode`) are deliberately
// NOT folded in here. Their lifetime is the odd one out — the preference is
// settable BEFORE a run and must survive the disabled window to be applied by
// `dc on`, and toggling it live is coupled to daze-immunity + camp-seed side
// effects (DungeonClearChatActions::ApplyPullSetting). They are already funneled
// through ApplyPullSetting / DisableDungeonClear and are excluded from every
// blanket reset anyway, so folding them here would add surface without any
// reset-safety gain. They stay as their own values.
struct DcRunState
{
    // === run session — cleared by Reset() (dc on / dc off / death / all-cleared) ===
    bool        enabled = false;   // the run's master switch (leader-owned)
    bool        paused  = false;   // soft-stop layered on `enabled`; see OnResume

    // Short human phrase describing WHY the run is paused, for the status panel to
    // tell a manual `dc pause` apart from a door auto-pause. Set at each pause site
    // the moment `paused` flips true; read only while paused. Empty falls back to a
    // generic "holding position".
    std::string pauseReason;

    // GUID of the closed door the tank auto-paused in front of (empty unless paused
    // specifically for an unopenable door). While set, DungeonClearDoorReopenedTrigger
    // polls this one door; the moment it reads OPEN the clear auto-resumes. Stamped
    // ONLY by the door auto-pause site — a manual `dc pause` leaves it empty so an
    // unrelated door can never auto-resume a hand-held pause.
    ObjectGuid  pausedDoor;

    // Boss entry of a manual boss override (0 = no override; normal auto progression).
    // Set by DcGoAction, cleared by dc on / dc off / dc skip.
    uint32      selectedBossEntry = 0;

    // === cross-bot leader-fight signals (were the g_* file-static maps) ============
    // Both are keyed, in the old design, by the LEADER's GUID and only ever read/
    // written for the resolved leader — i.e. they are facets of the leader's run.
    // Folded here they drop their standalone mutexes: all members of one group tick
    // on the same map thread, so a follower writing the leader's DcRunState is the
    // same single-threaded cross-bot access DcPullContext already relies on.

    // getMSTime() at which the leader's CURRENT continuous combat began (0 = out of
    // combat). Maintained lazily on read by DcLeaderSignal::LeaderCombatSince so the
    // threat-lead window measures from a FRESH combat start on the Leeroy / walk-in /
    // general-assist path (which has no pull-phase transition to mark fight start).
    // Was g_leaderCombatSince.
    uint32 leaderCombatSinceMs = 0;

    // getMSTime() of the last positive "some party member is in combat" observation,
    // the hysteresis latch behind IsPartyEngagedLatched (absorbs a one-tick combat
    // gap so the party doesn't snap out of "assist" mode mid-fight). 0 = never seen
    // engaged. Was g_partyEngagedLatch.
    uint32 partyEngagedLatchMs = 0;

    // === Smart Rest hysteresis latch (leader-owned, read cross-bot) ================
    // Maintained by DcSmartRest::UpdateLatch from the leader's between-pulls gate;
    // followers read it through the party tank (DcSmartRest::IsLatched). Combat does
    // NOT clear it — a patrol interrupting the rest goes inert (out-of-combat
    // triggers), then the still-set latch resumes the rest afterwards. The timeout
    // clock deliberately spans such interruptions.
    bool   smartRestLatched   = false;  // party is in a full-rest cycle
    uint32 smartRestSinceMs   = 0;      // getMSTime() when latched (timeout clock)
    uint32 smartRestRearmAtMs = 0;      // after a timeout release: no re-latch before this

    // Full run teardown: every session + signal field. Used on dc on / dc off /
    // death / all-cleared. (The pull preference/bool are NOT here — see the header
    // note; they are reset explicitly by ApplyPullSetting / DisableDungeonClear.)
    void Reset() { *this = DcRunState{}; }

    // Pause-cluster teardown — the resume path (manual `dc pause` resume AND the
    // door auto-resume) and re-arm on `dc on`. Clears the paused flag together with
    // the two fields that only mean anything while paused, so a stale reason/door
    // can never leak into the next pause. Boss progress is deliberately untouched —
    // that is the whole point of resume vs. a fresh `dc on`.
    void OnResume()
    {
        paused = false;
        pauseReason.clear();
        pausedDoor.Clear();
    }
};

#endif  // _PLAYERBOT_DCRUNSTATE_H
