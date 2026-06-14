/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearPullModeCurrentValue.h"

#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

bool DungeonClearPullModeCurrentValue::Calculate()
{
    // While a PERSISTENT anchored event drives (ZulFarrak's temple), the event
    // owns the tank: force the EFFECTIVE pull mode Off so the whole dynamic/
    // advanced pull system stands down as one — no camp-drag kiting the tank off
    // the waves, no scout-lag stranding the party up-ramp (scout-lag reads the
    // pull setting directly; see DcLeaderSignal::IsLeaderDynamicScouting, gated the
    // same way). The tank engages directly and tanks in place; the party follows
    // close and the leader-fight assist brings it in. This is the single switch
    // that replaces per-mechanic suppressions — the event needs exactly "pull Off"
    // behaviour. The stored pull-setting preference is untouched (the addon status
    // still shows it, and it resumes the instant the event completes).
    if (DungeonEventExecutor::IsPersistentAnchoredEventActive(context))
        return false;

    // Refresh the Dynamic (pull setting == 2) verdict for THIS tick, then report
    // the behavioural bool. UpdateDynamicPullMode is a no-op for Off/On (where
    // DcPullAction owns the bool) and internally throttles the expensive
    // classification, so running it on every read is cheap and idempotent.
    DcPullPlanner::UpdateDynamicPullMode(botAI, context);
    return AI_VALUE(bool, "dungeon clear pull mode");
}
