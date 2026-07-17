/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

#include "GameObject.h"
#include "InstanceScript.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"

// --- Sneed (643): dynamic spawn from Sneed's Shredder (642) ---------------
// Sneed's Shredder is a regular creature (no instance_encounter entry).
// When killed, Sneed (643, encounter bit 1) spawns from the wreckage as a
// TempSummon — invisible to the spawn-ID store scan that powers the boss
// tracker (GetCreatureBySpawnIdStore), so the live-boss detection never sees
// him and the engage engine never targets him. The party kills the shredder,
// Sneed attacks a DPS bot, but the tank advances toward Gilnid because the
// boss tracker says no boss is present at bit 1. This conditional event uses
// FindNearestCreature (grid search, finds summons) to detect and engage him.

namespace
{
    constexpr uint32 DM_SNEED             = 643;
    // The shredder's spawn point — Sneed spawns at the same position.
    constexpr float DM_SHREDDER_X         = -289.453f;
    constexpr float DM_SHREDDER_Y         = -513.009f;
    constexpr float DM_SHREDDER_Z         = 49.6785f;

    bool DmSneed(Player* bot, AiObjectContext* /*context*/)
    {
        // Only fire near the shredder arena. Avoids triggering from the
        // entrance or the foundry.
        if (bot->GetExactDist(DM_SHREDDER_X, DM_SHREDDER_Y, DM_SHREDDER_Z) > 120.0f)
            return false;

        // Sneed already dead — his encounter bit 1 is set. Let the normal
        // flow advance to Gilnid.
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (inst && (inst->GetCompletedEncounterMask() & (1u << 1)))
            return false;

        // Grid FindNearestCreature sees a TempSummon; the spawn-ID store
        // does not.
        return bot->FindNearestCreature(DM_SNEED, 80.0f, /*alive*/ true) != nullptr;
    }
}

// --- The Deadmines (map 36) — the DEFIAS CANNON, ANCHORED + PERSISTENT ---
// The way from the goblin foundry to the pirate ship is sealed by the Iron Clad
// Door (GO 16397, lock 202 — rogue-pick only, so a bot party can never click it
// open). The intended opening: loot the Defias Gunpowder chest (GO 17155) for
// the gunpowder, then use it on the Defias Cannon (GO 16398) — that casts spell
// 6250 at the cannon, whose SmartGameObjectAI fires it: the door swings to
// GO_STATE_ACTIVE_ALTERNATIVE ("closed door open by cannon fire"), two Defias
// Squallshapers are summoned, and the cannon instance data is set.
//
// Navigation is by the boss list: the cannon is added as an OBJECTIVE anchor
// (BossRosterRegistry, encounterIndex 3 — shared with Mr. Smite, the first ship
// boss; the objective-before-boss tie-break runs it first). It gates the way
// between Gilnid (bit 2, last foundry boss) and the whole ship (Mr. Smite bit 3,
// Cookie 4, Greenskin 5, VanCleef 6). Boss-nav drives the tank to the cannon
// exactly as to any boss; the event then fires it in place.
//
// The cannon's lock (83) is an ITEM lock requiring the Defias Gunpowder (item
// 5397) as the key, so spell 6250 only fires the cannon when cast FROM that
// item. A bot never loots the chest, so the FireDefiasCannon hook grants the
// gunpowder and uses it on the cannon (the item-use path supplies the cast-item
// the OPEN_LOCK needs). The gunpowder has a 2s "Opening" cast. See
// ObjectiveHookRegistry.
//
// PERSISTENT: firing the cannon summons two Squallshapers that immediately
// engage, so the event sees a >1s Drive gap on the combat engine right after the
// Custom step. A non-persistent event would rewind to step 0 across that gap and
// re-fire the cannon (summoning two more each time); persistence keeps the step
// progress so the cannon fires exactly once.

void RegisterDeadminesEvents(std::vector<DungeonEvent>& out)
{
    // Sneed — two-phase shredder/summon boss. Handled first (event id 2)
    // so it fires before the cannon event wherever Sneed is alive.
    out.push_back(
        EventBuilder(36, 2, "Kill Sneed")
            .Conditional(&DmSneed)
            .KillCreatureEngage(DM_SNEED, /*count*/ 1, /*searchRadius*/ 100.0f)
            .Build());

    constexpr uint32 DM_GO_IRON_CLAD_DOOR = 16397;
    constexpr uint32 DM_DOOR_OPEN_STATE   = 2;  // GO_STATE_ACTIVE_ALTERNATIVE
    constexpr uint32 DM_FIRE_CANNON_HOOK  = 2;  // ObjectiveHookRegistry id

    out.push_back(
        EventBuilder(36, 1, "Iron Clad Door (Defias Cannon)")
            .Anchored(/*encounterIndex*/ 3)
            .Persistent()
            // 1. Step up onto the cannon (the objective's arrive radius may
            //    leave the tank ~10yd off; the cannon GO is at this spot).
            .MoveTo(-107.56f, -659.67f, 7.21f, /*radius*/ 5.0f)
            // 2. Fire it. The hook casts spell 6250 at the cannon GO -> its
            //    SmartAI opens the door + summons the Squallshapers.
            .Custom(DM_FIRE_CANNON_HOOK)
            // 3. Hold until the Iron Clad Door is confirmed open, then the clear
            //    advances through it to Mr. Smite and the rest of the ship.
            .WaitForGOState(DM_GO_IRON_CLAD_DOOR, DM_DOOR_OPEN_STATE)
            .Timeout(30000)
            .Build());
}

// --- roster patch (relocated from BossRosterRegistry) --------------------
void RegisterDeadminesRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // --- The Deadmines (map 36) — Defias Cannon door ------------------
    // The Iron Clad Door (GO 16397, lock 202 — rogue-pick only) seals the
    // foundry from the pirate ship; a bot party can never click it open.
    // Add the Defias Cannon (GO 16398) as an OBJECTIVE anchor so boss-nav
    // drives the tank to it between Gilnid (bit 2, last foundry boss) and
    // Mr. Smite (bit 3, first ship boss); the event (eventId 1) fires the
    // cannon, opening the door to the whole ship (Smite 3, Cookie 4,
    // Greenskin 5, VanCleef 6). encounterIndex 3 is shared with Mr. Smite:
    // the objective-before-boss tie-break + the picker's strictly-greater
    // advance hand the objective over first, then the boss. No gateEntry
    // (the event owns completion via the door-open gate; see
    // DeadminesEvents).
    {
        BossRosterPatch p;
        p.mapId = 36;
        p.add = {
            MakeObjective(OBJ(1), /*encounterIndex*/ 3, 36, "Iron Clad Door",
                          -107.56f, -659.67f, 7.21f, /*arriveRadius*/ 10.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
        };
        t.push_back(std::move(p));
    }
}
