/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "GameObject.h"
#include "InstanceScript.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

// --- Razorfen Downs (map 129) — the GONG, CONDITIONAL + REPEATABLE --------
// The first thing a party does in RFD: ring the gong (GO 148917) by the
// entrance to summon Tuten'kash. The encounter is staged — each ring summons a
// wave (Tomb Fiends, then Tomb Reavers) and after the THIRD ring Tuten'kash
// himself spawns. The gong's SmartAI disables it (GO_FLAG_NOT_SELECTABLE) the
// instant it is rung and re-enables it only once the wave's deaths tick its
// kill-counter — so the gong is self-pacing: a bot just rings it whenever it is
// selectable again, and the server decides when the next wave (or the boss)
// appears.
//
// Navigation is done by Tuten'kash's BOSS anchor, which sits ON the gong
// (BossRosterRegistry): the tank travels to the gong exactly as it would to any
// boss — the long-range pathfinder, the dynamic-pull camps and the combat
// handling all drive it, which a bespoke event move cannot (an event-driven
// long haul fights the pull camp and never resets it — the live failure that
// sank the earlier LongMoveTo). The event therefore needs NO movement of its
// own; it only rings.
//
// The gong is rung in place. Condition 4 gates the ring on the tank being AT
// the gong (within range), the gong being selectable, and Tuten'kash absent —
// so it fires only once boss-nav has delivered the tank, preempting the
// boss-not-present stall (relevance 31 > 20). The anchor and the ring target are
// the SAME point (the gong), so there is no tug-of-war between boss-nav and the
// event.
//
// REPEATABLE so it re-fires for each of the three rings instead of latching
// after the first; the gong's own selectable flag (condition 4) paces the rings
// and Tuten'kash going live ends the loop. When he spawns (at his summon spot
// ~80yd off) the condition goes false and the normal boss flow engages him via
// live-boss tracking. Rendered first in the panel (PanelBeforeBoss 7355) since
// it precedes boss #0.

void RegisterRazorfenDownsEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(129, 1, "Ring the Gong")
                      .Conditional(4)
                      .Repeatable()
                      .PanelBeforeBoss(/*Tuten'kash*/ 7355)
                      // searchRadius must cover condition 4's ring range (30yd)
                      // so a tank parked at the far edge still finds the gong and
                      // HopTos the last yards to Use() it.
                      .UseGO(/*gong*/ 148917, /*searchRadius*/ 35.0f)
                      .Build());
}

// --- The gong activation condition (condition 4, repeatable) --------------
// The gong (GO 148917) summons Tuten'kash in three rings, a wave of trash
// between each. The gong's SmartAI flips it NOT_SELECTABLE the instant it is
// rung and re-enables it only once that wave's deaths tick its kill-counter, so
// it paces itself. This condition is the gate for the REPEATABLE "Ring the Gong"
// event: DUE while the party should ring again.
//
// Travel to the gong is NOT the event's job — Tuten'kash's boss anchor sits on
// the gong, so the boss navigation (pathfinder + dynamic-pull camps + combat)
// delivers the tank there exactly as to any boss. This gate therefore also
// requires the tank to already be AT the gong: that way the event fires only
// after boss-nav has arrived (preempting the boss-not-present stall to ring),
// and never hijacks the long approach with an event-driven move.
namespace
{
    constexpr uint32 RFD_GONG = 148917;
    constexpr uint32 RFD_TUTENKASH = 7355;
    // Tuten'kash is DungeonEncounter bit 0 (the map's first encounter), so his
    // kill flips bit 0 of the completed-encounter mask — the authoritative "the
    // gong event is finished" signal even after his corpse despawns.
    constexpr uint32 RFD_TUTENKASH_BIT = 0;
    // How close the tank must be to the gong for the ring to fire. Comfortably
    // larger than the boss engage range so the event takes over the instant
    // boss-nav has parked the tank at the gong, before the stall escalates.
    constexpr float RFD_GONG_RING_RANGE = 30.0f;

    bool RfdGong(Player* bot, AiObjectContext* /*context*/)
    {
        // Done for good once Tuten'kash is up (the 3rd ring summoned him — let
        // the boss flow take him) or already killed (his encounter bit is set).
        if (DcTargeting::FindLiveCreatureOnMap(bot, RFD_TUTENKASH))
            return false;
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (inst && (inst->GetCompletedEncounterMask() & (1u << RFD_TUTENKASH_BIT)))
            return false;

        // The gong must exist, be selectable (the previous wave is dead and its
        // SmartAI re-armed it) and shut. A NOT_SELECTABLE / already-activated
        // gong means a wave is still up — let combat finish before the next ring.
        GameObject* gong = bot->FindNearestGameObject(RFD_GONG, 200.0f);
        if (!gong)
            return false;
        if (gong->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) ||
            gong->GetGoState() != GO_STATE_READY)
            return false;

        // Only ring once boss-nav has parked the tank at the gong — otherwise the
        // event would preempt the boss travel partway and try to drive it itself.
        if (!bot->IsWithinDist(gong, RFD_GONG_RING_RANGE))
            return false;

        return true;
    }
}

void RegisterRazorfenDownsConditions(EventConditionMap& out)
{
    out[4] = &RfdGong;
}
