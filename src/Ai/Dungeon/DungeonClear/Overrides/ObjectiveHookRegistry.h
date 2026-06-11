/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_OBJECTIVEHOOKREGISTRY_H
#define _PLAYERBOT_OBJECTIVEHOOKREGISTRY_H

#include <functional>

#include "Common.h"

class Player;
class AiObjectContext;
struct DungeonBossInfo;

// Outcome of an objective's on-arrival hook.
enum class ObjectiveArriveResult
{
    Done,     // objective satisfied — latch it cleared and advance the clear
    Running,  // still working (e.g. mid-interaction) — hold and call again next tick
    Blocked,  // cannot complete unaided (needs the human) — stall the run
};

// Optional per-objective behaviour run when the tank reaches a travel objective
// (DungeonBossInfo::onArriveHook indexes this registry). The DEFAULT — no hook,
// id 0, or an unregistered id — is `Done`: the tank simply arrives and the clear
// advances. Registering a hook lets an objective DO something on arrival (use a
// gameobject, wait for a spawn, cast, etc.); the hook returns Running until it's
// finished and the tank holds at the anchor meanwhile.
//
// Hooks live in a hardcoded table (link-safe in the static-lib build, like the
// other DungeonClear registries). The table ships empty with a documented
// example; add a row and reference its id from a BossRosterRegistry objective.
class ObjectiveHookRegistry
{
public:
    using Hook = std::function<ObjectiveArriveResult(Player*, AiObjectContext*, DungeonBossInfo const&)>;

    // Runs the hook for `hookId`. Returns Done when `hookId` is 0 or unregistered
    // (the arrive-then-continue default).
    static ObjectiveArriveResult Run(uint32 hookId, Player* bot, AiObjectContext* context,
                                     DungeonBossInfo const& info);
};

#endif
