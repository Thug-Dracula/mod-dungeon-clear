/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "EventConditionRegistry.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

namespace
{
    // conditionId -> predicate. The predicates themselves live alongside the
    // events they gate, one file per dungeon under Data/Events/ (with shared,
    // cross-dungeon conditions in SharedConditions.cpp). Each file exposes a
    // Register<Dungeon>Conditions appender; this aggregator calls every one to
    // assemble the map. The explicit calls keep each per-dungeon TU linked into
    // the module static lib (a self-registering static initializer would be
    // dropped). The id space is documented in DungeonEventTables.h — keep it
    // collision-free.
    EventConditionMap const& Conditions()
    {
        static EventConditionMap const kConditions = []
        {
            EventConditionMap m;
            RegisterSharedEventConditions(m);
            RegisterShadowfangKeepConditions(m);
            RegisterRazorfenDownsConditions(m);
            RegisterStratholmeConditions(m);
            RegisterUldamanConditions(m);
            return m;
        }();
        return kConditions;
    }
}

bool EventConditionRegistry::Evaluate(uint32 id, Player* bot, AiObjectContext* context)
{
    if (id == 0 || !bot)
        return false;

    auto const& conditions = Conditions();
    auto it = conditions.find(id);
    if (it == conditions.end() || !it->second)
        return false;

    return it->second(bot, context);
}

bool EventConditionRegistry::Has(uint32 id)
{
    return id != 0 && Conditions().count(id) != 0;
}
