/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMultiplier.h"

#include "Action.h"
#include "FollowActions.h"
#include "Playerbots.h"

float DungeonClearMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;
    // While paused, behave as if DC were off: don't suppress wandering, so the
    // tank reverts to stock non-combat behavior just like under `dc off`.
    if (!botAI || !AI_VALUE(bool, "dungeon clear enabled") || AI_VALUE(bool, "dungeon clear paused"))
        return 1.0f;

    // The tank leads the clear — it must never follow its master. When Advance
    // yields to wait for the party to catch up (party spread > DC_PARTY_MAX_SPREAD)
    // it StopMoving()s and parks; without this, the stock FollowAction (relevance
    // 1.0) then wins the idle tick and walks the tank BACK toward the stationary
    // player, who is now in range again, so next tick Advance runs it forward to
    // the spread limit and it yields again — a rubberband between the spread limit
    // and the player. Suppressing follow for the tank lets it simply hold at the
    // spread limit until the player catches up. Followers are unaffected: their
    // redirect (DungeonClearFollowTankAction) is a separate MovementAction, and
    // only non-tanks run it.
    if (PlayerbotAI::IsTank(bot) && dynamic_cast<FollowAction*>(action))
        return 0.0f;

    std::string const& name = action->getName();
    // Suppress wander-style actions while DC is active. Anything else
    // (loot, food, drink, combat, our own dungeon-clear actions, and follow
    // for non-tanks) is untouched.
    if (name.find("grind") != std::string::npos)
        return 0.0f;
    if (name.find("rpg") != std::string::npos)
        return 0.0f;
    if (name == "travel" || name.find("travel ") != std::string::npos)
        return 0.0f;
    return 1.0f;
}
