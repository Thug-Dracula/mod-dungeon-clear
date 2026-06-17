/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "Creature.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "Log.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

// --- Uldaman (map 70) — the Ironaya seal, CONDITIONAL --------------------
// Ironaya (creature 7228, DungeonEncounter bit 2) sits behind the Seal of
// Khaz'Mul (GO 124372, lockId 0), which is opened only by interacting with the
// Keystone (GO 124371). Verified mechanic (DB + DBC + core, 2026-06-17):
//
//   * The keystone's Lock.dbc lock 359 = ITEM slot, item 7733 "Staff of
//     Prehistoria" — the player normally clicks the keystone holding the staff.
//   * The keystone runs SmartAI on SMART_EVENT_GOSSIP_HELLO (smart_scripts,
//     source_type 1, entry 124371): it opens the seal (124372 -> GO_STATE_ACTIVE),
//     sets DATA_IRONAYA_DOORS=DONE, and STRIPS Ironaya's unit_flags 33554434
//     (UNIT_FLAG_NON_ATTACKABLE | bit25) so she becomes attackable and walks out.
//   * GameObject::Use(bot) fires that GOSSIP_HELLO chain server-side with NO lock
//     check (the gossip-hello path runs before the type switch), so a plain
//     UseGO(124371) triggers everything — the staff is not functionally required.
//   * The seal (124372) is already excluded from BotCanOpenDoorLikePlayer's
//     force-open (lock-free, not allowlisted in DcEventDoorRegistry), so only
//     this event opens it.
//
// Anchored ordering can't slot this (it must run between DBC bits 1 and 2, with
// no integer order between them), so it is CONDITIONAL like Shadowfang Keep's
// courtyard door: relevance-31 DcRunEventAction preempts the boss pull and the
// door-blocked stall. The conditional driver drives the ClearRadius engage
// pipeline then Drive()s the keystone/wait steps. Required — Ironaya is a
// mandatory spine boss, so a non-firing seal should stall for the human rather
// than silently skip into a dead-end at the shut door.

namespace
{
    constexpr uint32 ULD_KEYSTONE  = 124371;
    constexpr uint32 ULD_SEAL_DOOR = 124372;
    constexpr uint32 ULD_IRONAYA   = 7228;
    // Proximity gate to the antechamber; the GO/creature are co-loaded so this
    // doubles as a grid-safety check.
    constexpr float  ULD_SCAN      = 120.0f;

    // The Ironaya antechamber's clear centre — between the south entry ramp
    // (y ~198) and the seal (y ~295). Radius covers all Stonevault trash
    // (x -267..-210, y 198..286) and stays SHORT of the sealed Ironaya
    // (-235.7, 309.6, 61yd from centre) so the sweep never targets her.
    constexpr float  ULD_ROOM_X      = -235.0f;
    constexpr float  ULD_ROOM_Y      = 248.0f;
    constexpr float  ULD_ROOM_Z      = -48.0f;
    constexpr float  ULD_ROOM_RADIUS = 55.0f;
    constexpr float  ULD_ROOM_ZBAND  = 20.0f;
    constexpr uint32 ULD_ROOM_TIMEOUT = 120000;  // give-up valve on the pre-clear

    // Activation: due while the party is near the still-shut seal and Ironaya is
    // present (always spawned, just sealed). Latches DONE on completion, and also
    // reads false the instant the seal opens, so it fires exactly once.
    bool UldamanIronayaSeal(Player* bot, AiObjectContext* /*context*/)
    {
        GameObject* seal = bot->FindNearestGameObject(ULD_SEAL_DOOR, ULD_SCAN);
        Creature* ironaya = bot->FindNearestCreature(ULD_IRONAYA, ULD_SCAN, /*alive*/ true);

        // Throttled diagnostic (single-threaded world tick): one line / 5s so a
        // live run shows WHY the event is/ isn't due. Lands in DungeonClear.log.
        static uint32 lastLog = 0;
        uint32 const now = getMSTime();
        if (getMSTimeDiff(lastLog, now) >= 5000)
        {
            lastLog = now;
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] Uldaman Ironaya seal cond: seal={} state={} ironaya={}",
                      bot->GetName(), seal ? "found" : "MISSING",
                      seal ? static_cast<int>(seal->GetGoState()) : -1,
                      ironaya ? "present" : "no");
        }

        if (!seal)
            return false;                            // not near the chamber yet
        if (seal->GetGoState() != GO_STATE_READY)
            return false;                            // already open -> done
        return ironaya != nullptr;
    }
}

void RegisterUldamanEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(70, 1, "Unseal Ironaya (Seal of Khaz'Mul)")
                      .Conditional(8)
                      // Render in the panel just before Ironaya (cosmetic; does not
                      // affect engine ordering — the seal is opened on her gate).
                      .PanelBeforeBoss(ULD_IRONAYA)
                      // 1) clear the antechamber (sealed Ironaya is out of range).
                      .ClearRadius(ULD_ROOM_X, ULD_ROOM_Y, ULD_ROOM_Z,
                                   ULD_ROOM_RADIUS, ULD_ROOM_ZBAND)
                      .Timeout(ULD_ROOM_TIMEOUT)
                      // 2) step onto the keystone (a short hop south of the room).
                      .MoveTo(-234.69f, 239.62f, -50.91f, /*radius*/ 5.0f)
                      // 3) use the keystone -> SmartAI opens the seal + unseals
                      //    Ironaya. UseGO is idempotent (skips an already-used GO),
                      //    so a combat tick-gap re-running the step list is safe.
                      .UseGO(ULD_KEYSTONE, /*searchRadius*/ 10.0f)
                      // 4) wait for the seal to open. The keystone's SmartAI opens
                      //    it on a 27s CREATE_TIMED_EVENT (the door "rumbles open"
                      //    delay), NOT synchronously on the click — so the timeout
                      //    must clear 27s with margin (live-verified: a 10s timeout
                      //    spuriously "stalled" until Ironaya unsealed at +27s).
                      .WaitForGOState(ULD_SEAL_DOOR, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 45000)
                      .Build());
}

void RegisterUldamanConditions(EventConditionMap& out)
{
    out[8] = &UldamanIronayaSeal;
}
