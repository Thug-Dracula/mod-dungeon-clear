/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_ROOMAGGROREGISTRY_H
#define _PLAYERBOT_ROOMAGGROREGISTRY_H

#include <vector>

#include "Define.h"

// Static registry of bosses that, on engage, force the surrounding ROOM into
// combat beyond ordinary social aggro — a scripted CallForHelp / grid-list
// AttackStart / SmartAI CALL_FOR_HELP bounded by a radius (± an entry
// whitelist). For these encounters the tank must clear the room BEFORE pulling
// the boss, or the group eats the whole pile when the pull goes off. See
// deployment-files/docs/mod-dungeon-clear_room-aggro-bosses_plan.md.
//
// One model only: the "room" is the script's own radius-sphere around the LIVE
// boss (some flagged bosses wander). Whole-instance pulls (SetInCombatWithZone
// on the boss itself) are deliberately NOT in this registry — a whole zone
// cannot be pre-cleared.
struct RoomAggroBoss
{
    uint32 mapId{0};
    uint32 bossEntry{0};
    float  radius{0.0f};                 // the script's own R (yd), from source
    std::vector<uint32> memberEntries;   // script's entry whitelist; EMPTY = any
                                         // hostile in radius (the safe
                                         // over-approximation — see the plan)
};

class RoomAggroRegistry
{
public:
    // The flagged-boss row for (mapId, bossEntry), or nullptr when the boss is
    // not a room-aggro boss. Linear scan — the table is small.
    static RoomAggroBoss const* Find(uint32 mapId, uint32 bossEntry);

    // True when `entry` participates in the boss's room pull: either the boss
    // has no whitelist (any hostile counts) or `entry` is on the whitelist.
    // Pure — no game state — so it is unit-testable on its own.
    static bool IsMemberEntry(RoomAggroBoss const& boss, uint32 entry);

    // Pure geometric membership predicate for one candidate creature, factored
    // out of DungeonClearRoomTrashValue so it can be tested without a live map.
    // True when the candidate (entry `entry`, `distToBoss` yards from the live
    // boss) is room trash that should be cleared first: it is a whitelist
    // member, sits inside the room radius, AND sits OUTSIDE the boss's own
    // aggro sphere (`bossSafeRadius`). Units inside the boss's aggro sphere
    // cannot be pulled without waking the boss, so they are treated as "coming
    // with the boss" and excluded — never counted as remaining room trash, or
    // the gate would livelock on a mob it can never separate.
    static bool IsRoomTrash(RoomAggroBoss const& boss, uint32 entry,
                            float distToBoss, float bossSafeRadius);
};

#endif
