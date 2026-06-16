/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONEVENTTABLES_H
#define _PLAYERBOT_DUNGEONEVENTTABLES_H

#include <unordered_map>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/EventConditionRegistry.h"

// Internal registration seam for the per-dungeon event tables.
//
// Each dungeon owns one .cpp in this folder that defines its event rows
// (Register<Dungeon>Events) and, when it has conditional events, its activation
// predicates (Register<Dungeon>Conditions). The two central registries
// (DungeonEventRegistry / EventConditionRegistry) call these aggregators
// EXPLICITLY so every per-dungeon translation unit stays referenced.
//
// Why explicit calls and not self-registering static initializers: the module
// compiles into a static lib, and a TU whose only output is constructor
// side-effects (with no symbol the program references) is dropped by the linker
// — its events would silently vanish. The one-big-table this replaces avoided
// that by keeping everything in a single referenced TU; the aggregator calls
// below restore the reference chain per file. (Same reason ObjectiveHookRegistry
// and friends use hardcoded tables.)
//
// Adding a dungeon:
//   1. Create <Dungeon>Events.cpp here.
//   2. Define Register<Dungeon>Events (and Conditions, if it has any).
//   3. Declare them below.
//   4. Add the call in the matching aggregator (EventTable() in
//      DungeonEventRegistry.cpp / Conditions() in EventConditionRegistry.cpp).
//
// Condition id allocation — the global id space referenced by .Conditional(id).
// Keep this list authoritative when adding rows so ids never collide:
//   1  Shadowfang Keep  — courtyard door, Alliance
//   2  Shadowfang Keep  — courtyard door, Horde
//   3  (shared)         — room-aggro pre-clear (every RoomAggroRegistry boss)
//   4  Razorfen Downs   — the gong
//   -- next free: 5
using EventConditionMap =
    std::unordered_map<uint32, EventConditionRegistry::Condition>;

// --- event rows (one appender per dungeon) -------------------------------
void RegisterSunkenTempleEvents(std::vector<DungeonEvent>& out);
void RegisterZulFarrakEvents(std::vector<DungeonEvent>& out);
void RegisterShadowfangKeepEvents(std::vector<DungeonEvent>& out);
void RegisterScarletMonasteryEvents(std::vector<DungeonEvent>& out);
void RegisterRazorfenDownsEvents(std::vector<DungeonEvent>& out);
void RegisterBlackrockDepthsEvents(std::vector<DungeonEvent>& out);
void RegisterDeadminesEvents(std::vector<DungeonEvent>& out);

// --- activation conditions (only for dungeons with Conditional events) ----
void RegisterSharedEventConditions(EventConditionMap& out);
void RegisterShadowfangKeepConditions(EventConditionMap& out);
void RegisterRazorfenDownsConditions(EventConditionMap& out);

#endif
