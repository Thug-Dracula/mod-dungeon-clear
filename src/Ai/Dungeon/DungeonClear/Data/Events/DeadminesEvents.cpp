/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

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
// A bot never loots the chest or uses the gunpowder, so the FireDefiasCannon
// hook replays the authentic spell hit directly (CastSpell at the cannon GO).
// See ObjectiveHookRegistry.
//
// PERSISTENT: firing the cannon summons two Squallshapers that immediately
// engage, so the event sees a >1s Drive gap on the combat engine right after the
// Custom step. A non-persistent event would rewind to step 0 across that gap and
// re-fire the cannon (summoning two more each time); persistence keeps the step
// progress so the cannon fires exactly once.

void RegisterDeadminesEvents(std::vector<DungeonEvent>& out)
{
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
