/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcTickMemo.h"

#include "DcEngageGeometry.h"
#include "DcPartyState.h"
#include "DungeonClearTuning.h"   // DC_ENGAGE_RANGE
#include "Timer.h"
#include "AiObjectContext.h"
#include "Value.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

bool DcTickMemo::MemoValid(std::uint32_t stampMs, std::uint32_t now)
{
    if (stampMs == 0)
        return false;
    return getMSTimeDiff(stampMs, now) <= kMemoWindowMs;
}

void DcTickMemo::EnsureFresh(std::uint32_t now)
{
    if (MemoValid(stampMs, now))
        return;
    *this = DcTickMemo{};       // clear all cached fields
    stampMs = now ? now : 1;    // never stamp 0 (== "never filled")
}

namespace
{
    DcTickMemo& Memo(AiObjectContext* ctx)
    {
        DcTickMemo& m =
            ctx->GetValue<DcTickMemo&>(DcKey::TickMemo)->Get();
        m.EnsureFresh(getMSTime());
        return m;
    }
}

bool DcTickMemoAccess::AtBossEngage(Player* bot, AiObjectContext* ctx,
                                    DungeonBossInfo const& next)
{
    if (!bot || !ctx)
        return DcEngageGeometry::IsAtBossEngage(bot, ctx, next, DC_ENGAGE_RANGE);

    DcTickMemo& m = Memo(ctx);
    if (m.atBossEngage < 0)
        m.atBossEngage =
            DcEngageGeometry::IsAtBossEngage(bot, ctx, next, DC_ENGAGE_RANGE) ? 1 : 0;
    return m.atBossEngage == 1;
}

bool DcTickMemoAccess::BetweenPullsReady(Player* bot, AiObjectContext* ctx,
                                         bool requireNoLoot)
{
    if (!bot || !ctx)
        return DcPartyState::IsBetweenPullsReady(bot, ctx, requireNoLoot);

    DcTickMemo& m = Memo(ctx);
    std::int8_t& slot =
        requireNoLoot ? m.betweenPullsReadyStrict : m.betweenPullsReadyLoose;
    if (slot < 0)
        slot = DcPartyState::IsBetweenPullsReady(bot, ctx, requireNoLoot) ? 1 : 0;
    return slot == 1;
}
