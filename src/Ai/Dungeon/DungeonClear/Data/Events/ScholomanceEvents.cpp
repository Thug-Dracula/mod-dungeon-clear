/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- Scholomance (map 289) — Marduk & Vectus ROOM-AGGRO PRE-CLEAR ----------
// Marduk Blackpool (10433) and Vectus (10432) are a linked pair (pulling either
// pulls both, see their SmartAI) collapsed into ONE boss anchored on Vectus by
// BossRosterRegistry (map 289). They sit in a chamber of ~32 Scholomance
// Students; though they do not pre-aggro, an AoE that wakes one wakes the pair
// while the room is still up. Re-use the shared room-aggro pre-clear model
// (condition 3 — DUE while DungeonClearRoomTrashValue still has room trash, with
// the RoomAggroRegistry geometry around the LIVE boss minus the boss aggro
// sphere / unreachable / door-blocked): the lone KillCreature(0 = room-trash
// mode) step gates the boss pull until the students are cleared, and
// DcRunEventAction drives the engage (nearest room trash first). Required (hold
// the pull until clear or the value gives up) and NEVER latched — the same row
// re-fires for the merged boss. Both 10432 and 10433 are in RoomAggroRegistry so
// the tracked boss's keep-away sphere protects the close trash and Marduk (off
// the roster) is excluded from being chased as a faction-15 student.

void RegisterScholomanceEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(289, 1, "Clear the room (Marduk & Vectus)")
                      .Conditional(3)
                      .KillCreature(/*room trash*/ 0)
                      .Build());
}
