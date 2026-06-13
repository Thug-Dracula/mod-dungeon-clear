/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- Scarlet Monastery Cathedral (map 189) — ROOM-AGGRO PRE-CLEAR ---------
// Highlord Mograine's engage fires a grid AttackStart that drags the WHOLE
// cathedral into combat (CATHEDRAL_PULL_RANGE 80); pulling him before the room
// is clear wipes the party. Milestone 3 re-expresses that pre-clear (formerly
// the standalone RoomAggroRegistry path) as a CONDITIONAL room-aggro event:
// condition 3 (shared — see SharedConditions.cpp) reads DUE while the room-trash
// value (the RoomAggroRegistry geometry around the LIVE boss, minus the boss
// aggro sphere / unreachable / door-blocked, with the RoomClearTimeout give-up
// valve) still has anything to clear, and the lone KillCreature(0 = room-trash
// mode) step gates the boss pull until it is empty. DcRunEventAction drives the
// actual engage (nearest room trash first); the spatial logic stays in
// DungeonClearRoomTrashValue. Required (hold the pull until clear or the value
// gives up) and NEVER latched — the same row re-fires for each room-aggro boss
// on the map (Mograine, then Whitemane). This is the SM Cathedral test bed;
// other RoomAggroRegistry maps migrate by adding one more row each (and keep the
// legacy path until they do).

void RegisterScarletMonasteryEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(189, 1, "Clear the Cathedral (room-aggro pre-clear)")
                      .Conditional(3)
                      .KillCreature(/*room trash*/ 0)
                      .Build());
}
