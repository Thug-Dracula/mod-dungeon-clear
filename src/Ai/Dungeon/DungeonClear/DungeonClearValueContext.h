/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARVALUECONTEXT_H
#define _PLAYERBOT_DUNGEONCLEARVALUECONTEXT_H

#include "NamedObjectContext.h"
#include "Value.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonBossesValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearBlockingDoorValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearFarTargetsValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLongPathValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearPartyTankValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearStateValues.h"
#include "Ai/Dungeon/DungeonClear/Value/NextDungeonBossValue.h"

class DungeonClearValueContext : public NamedObjectContext<UntypedValue>
{
public:
    DungeonClearValueContext() : NamedObjectContext<UntypedValue>(false, false)
    {
        creators["dungeon bosses"] = &DungeonClearValueContext::dungeon_bosses;
        creators["next dungeon boss"] = &DungeonClearValueContext::next_dungeon_boss;
        creators["dungeon clear live boss"] = &DungeonClearValueContext::dungeon_clear_live_boss;
        creators["dungeon clear enabled"] = &DungeonClearValueContext::dungeon_clear_enabled;
        creators["dungeon clear paused"] = &DungeonClearValueContext::dungeon_clear_paused;
        creators["dungeon clear skipped"] = &DungeonClearValueContext::dungeon_clear_skipped;
        creators["dungeon clear seen bosses"] = &DungeonClearValueContext::dungeon_clear_seen_bosses;
        creators["dungeon clear stuck count"] = &DungeonClearValueContext::dungeon_clear_stuck_count;
        creators["dungeon clear last target entry"] = &DungeonClearValueContext::dungeon_clear_last_target_entry;
        creators["dungeon clear sticky boss"] = &DungeonClearValueContext::dungeon_clear_sticky_boss;
        creators["dungeon clear selected boss"] = &DungeonClearValueContext::dungeon_clear_selected_boss;
        creators["dungeon clear stall reason"] = &DungeonClearValueContext::dungeon_clear_stall_reason;
        creators["dungeon clear last said reason"] = &DungeonClearValueContext::dungeon_clear_last_said_reason;
        creators["dungeon clear phase"] = &DungeonClearValueContext::dungeon_clear_phase;
        creators["dungeon clear fallback target"] = &DungeonClearValueContext::dungeon_clear_fallback_target;
        creators["dungeon clear party tank"] = &DungeonClearValueContext::dungeon_clear_party_tank;
        creators["dungeon clear last position"] = &DungeonClearValueContext::dungeon_clear_last_position;
        creators["dungeon clear stuck ticks"] = &DungeonClearValueContext::dungeon_clear_stuck_ticks;
        creators["dungeon clear last pull target"] = &DungeonClearValueContext::dungeon_clear_last_pull_target;
        creators["dungeon clear long path"] = &DungeonClearValueContext::dungeon_clear_long_path;
        creators["dungeon clear long path target"] = &DungeonClearValueContext::dungeon_clear_long_path_target;
        creators["dungeon clear long path expires"] = &DungeonClearValueContext::dungeon_clear_long_path_expires;
        creators["dungeon clear current hop"] = &DungeonClearValueContext::dungeon_clear_current_hop;
        creators["dungeon clear pending path job"] = &DungeonClearValueContext::dungeon_clear_pending_path_job;
        creators["dungeon clear pending path since"] = &DungeonClearValueContext::dungeon_clear_pending_path_since;
        creators["dungeon clear far targets"] = &DungeonClearValueContext::dungeon_clear_far_targets;
        creators["dungeon clear blocking door"] = &DungeonClearValueContext::dungeon_clear_blocking_door;
        creators["dungeon clear engage trash target"] = &DungeonClearValueContext::dungeon_clear_engage_trash_target;
        creators["dungeon clear stride rebuild attempts"] = &DungeonClearValueContext::dungeon_clear_stride_rebuild_attempts;
        creators["dungeon clear follower state"] = &DungeonClearValueContext::dungeon_clear_follower_state;
        creators["dungeon clear loot yield start"] = &DungeonClearValueContext::dungeon_clear_loot_yield_start;
        creators["dungeon clear loot skip"] = &DungeonClearValueContext::dungeon_clear_loot_skip;
        creators["dungeon clear loot camp guid"] = &DungeonClearValueContext::dungeon_clear_loot_camp_guid;
        creators["dungeon clear loot camp start"] = &DungeonClearValueContext::dungeon_clear_loot_camp_start;
        creators["dungeon clear done-not-engaged ticks"] = &DungeonClearValueContext::dungeon_clear_done_not_engaged_ticks;
        creators["dungeon clear pursuit fail ticks"] = &DungeonClearValueContext::dungeon_clear_pursuit_fail_ticks;
        creators["dungeon clear followed tank"] = &DungeonClearValueContext::dungeon_clear_followed_tank;
    }

private:
    static UntypedValue* dungeon_bosses(PlayerbotAI* ai) { return new DungeonBossesValue(ai); }
    static UntypedValue* next_dungeon_boss(PlayerbotAI* ai) { return new NextDungeonBossValue(ai); }
    static UntypedValue* dungeon_clear_live_boss(PlayerbotAI* ai) { return new DungeonClearLiveBossValue(ai); }
    static UntypedValue* dungeon_clear_enabled(PlayerbotAI* ai) { return new DungeonClearEnabledValue(ai); }
    static UntypedValue* dungeon_clear_paused(PlayerbotAI* ai) { return new DungeonClearPausedValue(ai); }
    static UntypedValue* dungeon_clear_skipped(PlayerbotAI* ai) { return new DungeonClearSkippedValue(ai); }
    static UntypedValue* dungeon_clear_seen_bosses(PlayerbotAI* ai) { return new DungeonClearSeenBossesValue(ai); }
    static UntypedValue* dungeon_clear_stuck_count(PlayerbotAI* ai) { return new DungeonClearStuckCountValue(ai); }
    static UntypedValue* dungeon_clear_last_target_entry(PlayerbotAI* ai) { return new DungeonClearLastTargetEntryValue(ai); }
    static UntypedValue* dungeon_clear_sticky_boss(PlayerbotAI* ai) { return new DungeonClearStickyBossValue(ai); }
    static UntypedValue* dungeon_clear_selected_boss(PlayerbotAI* ai) { return new DungeonClearSelectedBossValue(ai); }
    static UntypedValue* dungeon_clear_stall_reason(PlayerbotAI* ai) { return new DungeonClearStallReasonValue(ai); }
    static UntypedValue* dungeon_clear_last_said_reason(PlayerbotAI* ai) { return new DungeonClearLastSaidReasonValue(ai); }
    static UntypedValue* dungeon_clear_phase(PlayerbotAI* ai) { return new DungeonClearPhaseValue(ai); }
    static UntypedValue* dungeon_clear_fallback_target(PlayerbotAI* ai) { return new DungeonClearFallbackTargetValue(ai); }
    static UntypedValue* dungeon_clear_party_tank(PlayerbotAI* ai) { return new DungeonClearPartyTankValue(ai); }
    static UntypedValue* dungeon_clear_last_position(PlayerbotAI* ai) { return new DungeonClearLastPositionValue(ai); }
    static UntypedValue* dungeon_clear_stuck_ticks(PlayerbotAI* ai) { return new DungeonClearStuckTicksValue(ai); }
    static UntypedValue* dungeon_clear_last_pull_target(PlayerbotAI* ai) { return new DungeonClearLastPullTargetValue(ai); }
    static UntypedValue* dungeon_clear_long_path(PlayerbotAI* ai) { return new DungeonClearLongPathValue(ai); }
    static UntypedValue* dungeon_clear_long_path_target(PlayerbotAI* ai) { return new DungeonClearLongPathTargetValue(ai); }
    static UntypedValue* dungeon_clear_long_path_expires(PlayerbotAI* ai) { return new DungeonClearLongPathExpiresValue(ai); }
    static UntypedValue* dungeon_clear_current_hop(PlayerbotAI* ai) { return new DungeonClearCurrentHopValue(ai); }
    static UntypedValue* dungeon_clear_pending_path_job(PlayerbotAI* ai) { return new DungeonClearPendingPathJobValue(ai); }
    static UntypedValue* dungeon_clear_pending_path_since(PlayerbotAI* ai) { return new DungeonClearPendingPathSinceValue(ai); }
    static UntypedValue* dungeon_clear_far_targets(PlayerbotAI* ai) { return new DungeonClearFarTargetsValue(ai); }
    static UntypedValue* dungeon_clear_blocking_door(PlayerbotAI* ai) { return new DungeonClearBlockingDoorValue(ai); }
    static UntypedValue* dungeon_clear_engage_trash_target(PlayerbotAI* ai) { return new DungeonClearEngageTrashTargetValue(ai); }
    static UntypedValue* dungeon_clear_stride_rebuild_attempts(PlayerbotAI* ai) { return new DungeonClearStrideRebuildAttemptsValue(ai); }
    static UntypedValue* dungeon_clear_follower_state(PlayerbotAI* ai) { return new DungeonClearFollowerStateValue(ai); }
    static UntypedValue* dungeon_clear_loot_yield_start(PlayerbotAI* ai) { return new DungeonClearLootYieldStartValue(ai); }
    static UntypedValue* dungeon_clear_loot_skip(PlayerbotAI* ai) { return new DungeonClearLootSkipValue(ai); }
    static UntypedValue* dungeon_clear_loot_camp_guid(PlayerbotAI* ai) { return new DungeonClearLootCampGuidValue(ai); }
    static UntypedValue* dungeon_clear_loot_camp_start(PlayerbotAI* ai) { return new DungeonClearLootCampStartValue(ai); }
    static UntypedValue* dungeon_clear_done_not_engaged_ticks(PlayerbotAI* ai) { return new DungeonClearDoneNotEngagedTicksValue(ai); }
    static UntypedValue* dungeon_clear_pursuit_fail_ticks(PlayerbotAI* ai) { return new DungeonClearPursuitFailTicksValue(ai); }
    static UntypedValue* dungeon_clear_followed_tank(PlayerbotAI* ai) { return new DungeonClearFollowedTankValue(ai); }
};

#endif
