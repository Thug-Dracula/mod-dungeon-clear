/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearPullTargetValue.h"

#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonBossesValue.h"

ObjectGuid DungeonClearPullTargetValue::Calculate()
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return ObjectGuid::Empty;

    // Sticky fast path: the governor's per-pack latch IS the target while it
    // still resolves to a valid pull. Deliberately does NOT require it to still
    // be the closest blocker nor to still sit inside the scan corridor — packs
    // drift while patrolling, and re-running corridor membership is exactly the
    // instability being removed. Death/abort/door/distance release it.
    DcPullContext const& pull =
        context->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    if (!pull.decisionTarget.IsEmpty())
    {
        Unit* sticky = ObjectAccessor::GetUnit(*bot, pull.decisionTarget);
        if (sticky && DcTargeting::IsStickyPullTargetValid(bot, context, sticky))
            return pull.decisionTarget;
    }

    std::optional<DungeonBossInfo> next =
        context->GetValue<std::optional<DungeonBossInfo>>("next dungeon boss")->Get();
    if (!next.has_value())
        return ObjectGuid::Empty;

    Unit* fresh = DcTargeting::FindPullTarget(botAI, *next);
    return fresh ? fresh->GetGUID() : ObjectGuid::Empty;
}
