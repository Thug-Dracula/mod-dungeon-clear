/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <unordered_map>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"

namespace
{
    // hookId -> behaviour. Ships EMPTY: every objective uses the arrive-then-
    // continue default. To give an objective on-arrival behaviour, add a row
    // here and set DungeonBossInfo::onArriveHook to its id in the roster patch.
    //
    // Example (do not enable without a real objective referencing it):
    //
    //   { 1, [](Player* bot, AiObjectContext* ctx, DungeonBossInfo const& info)
    //        {
    //            // ... interact with a gameobject / wait for a spawn ...
    //            // return ObjectiveArriveResult::Running while still working;
    //            // return ObjectiveArriveResult::Done once finished.
    //            return ObjectiveArriveResult::Done;
    //        }},
    std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const& Hooks()
    {
        static std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const kHooks = {};
        return kHooks;
    }
}

ObjectiveArriveResult ObjectiveHookRegistry::Run(uint32 hookId, Player* bot, AiObjectContext* context,
                                                 DungeonBossInfo const& info)
{
    if (hookId == 0)
        return ObjectiveArriveResult::Done;

    auto const& hooks = Hooks();
    auto it = hooks.find(hookId);
    if (it == hooks.end() || !it->second)
        return ObjectiveArriveResult::Done;

    return it->second(bot, context, info);
}
