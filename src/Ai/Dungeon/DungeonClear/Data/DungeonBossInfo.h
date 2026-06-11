/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONBOSSINFO_H
#define _PLAYERBOT_DUNGEONBOSSINFO_H

#include <string>

#include "Common.h"

// A boss list entry is either a real combat boss (the default — killed for
// encounter credit) or a non-combat "objective": a navmesh anchor the tank
// travels to in order to progress an event (trigger a spawn, traverse a gate),
// after which it is marked done and the clear advances. See BossRosterRegistry.
enum class DungeonAnchorKind : uint8
{
    Boss,
    Objective,
};

struct DungeonBossInfo
{
    uint32 entry{0};
    uint32 encounterIndex{0};
    std::string name;
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    // --- Roster-override extensions (defaulted; untouched for auto-derived
    //     bosses from BossSpawnIndex) ----------------------------------------

    // Boss vs travel-objective. Objectives skip the combat/engage machinery and
    // complete on arrival (see DcObjectiveArriveAction).
    DungeonAnchorKind kind{DungeonAnchorKind::Boss};

    // Objective only: arrival/completion radius in yards. 0 => use the
    // ObjectiveArriveRadius config default.
    float arriveRadius{0.0f};

    // Objective only: optional creature whose presence (alive) also satisfies
    // the objective — e.g. the real boss has spawned as a result of the event.
    uint32 gateEntry{0};

    // Objective only: id into ObjectiveHookRegistry for an on-arrival handler.
    // 0 => no hook; the objective simply completes and the clear advances.
    uint32 onArriveHook{0};

    // Boss only (roster patch authoring aid): copy this base entry's
    // encounterIndex onto this added boss so it borrows that boss's kill-bit
    // for completion detection. Resolved and cleared by BossRosterRegistry::Apply.
    uint32 inheritCompletionFrom{0};
};

#endif
