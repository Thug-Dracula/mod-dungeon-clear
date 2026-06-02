/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearPartyTankValue.h"

#include "Group.h"
#include "Player.h"
#include "Playerbots.h"

Player* DungeonClearPartyTankValue::Calculate()
{
    if (!bot)
        return nullptr;
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;
        if (!member->IsAlive())
            continue;
        if (member->GetMapId() != bot->GetMapId())
            continue;
        if (!PlayerbotAI::IsTank(member))
            continue;

        // Only bot tanks can have DC enabled. A real-player tank has no
        // PlayerbotAI, so the cross-context lookup below is skipped.
        PlayerbotAI* tankAI = GET_PLAYERBOT_AI(member);
        if (!tankAI)
            continue;

        // A paused tank counts as "no DC tank" so followers stop following it
        // and revert to the player — matching the tank's own paused behavior.
        AiObjectContext* tankCtx = tankAI->GetAiObjectContext();
        if (tankCtx->GetValue<bool>("dungeon clear enabled")->Get() &&
            !tankCtx->GetValue<bool>("dungeon clear paused")->Get())
            return member;
    }
    return nullptr;
}
