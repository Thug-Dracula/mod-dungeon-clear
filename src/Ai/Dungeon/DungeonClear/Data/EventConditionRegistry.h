/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_EVENTCONDITIONREGISTRY_H
#define _PLAYERBOT_EVENTCONDITIONREGISTRY_H

#include <functional>

#include "Common.h"

class Player;
class AiObjectContext;

// Predicate table for a Conditional dungeon EVENT's activation (DungeonEvent::
// conditionId). A Conditional event is not anchored into the boss-ordered list;
// instead the conditional-event trigger evaluates its condition each tick and,
// when true and the event is not yet latched, drives its steps (e.g. an off-path
// lever, or a "talk to the prisoner to open the gate" pre-boss gate).
//
// Conditions are pure-ish read-only predicates over the live world (bot/context)
// — never mutate state here. They live in a hardcoded table (link-safe in the
// static-lib build) exactly like ObjectiveHookRegistry. A conditionId of 0, or
// an unregistered id, evaluates to false (the event never fires — fail safe).
class EventConditionRegistry
{
public:
    using Condition = std::function<bool(Player*, AiObjectContext*)>;

    // Evaluate condition `id`. Returns false for id 0, an unregistered id, or a
    // null bot — so a mis-authored event simply never activates.
    static bool Evaluate(uint32 id, Player* bot, AiObjectContext* context);

    // True if `id` names a registered condition (cheap authoring sanity gate).
    static bool Has(uint32 id);
};

#endif
