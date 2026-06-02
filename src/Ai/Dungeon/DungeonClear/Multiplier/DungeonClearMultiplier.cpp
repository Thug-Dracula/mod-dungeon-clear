/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMultiplier.h"

#include "Action.h"
#include "Playerbots.h"

float DungeonClearMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;
    // While paused, behave as if DC were off: don't suppress wandering, so the
    // tank reverts to stock non-combat behavior just like under `dc off`.
    if (!botAI || !AI_VALUE(bool, "dungeon clear enabled") || AI_VALUE(bool, "dungeon clear paused"))
        return 1.0f;

    std::string const& name = action->getName();
    // Suppress wander-style actions while DC is active. Anything else
    // (loot, food, drink, follow, combat, our own dungeon-clear actions)
    // is untouched.
    if (name.find("grind") != std::string::npos)
        return 0.0f;
    if (name.find("rpg") != std::string::npos)
        return 0.0f;
    if (name == "travel" || name.find("travel ") != std::string::npos)
        return 0.0f;
    return 1.0f;
}
