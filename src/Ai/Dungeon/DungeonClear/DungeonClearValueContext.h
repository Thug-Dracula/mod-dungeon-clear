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
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearPullModeCurrentValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearHealTargetValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearPullTargetValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearRoomTrashValue.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearStateValues.h"
#include "Ai/Dungeon/DungeonClear/Value/NextDungeonBossValue.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

class DungeonClearValueContext : public NamedObjectContext<UntypedValue>
{
public:
    DungeonClearValueContext() : NamedObjectContext<UntypedValue>(false, false)
    {
        creators[DcKey::DungeonBosses] = &DungeonClearValueContext::dungeon_bosses;
        creators[DcKey::NextDungeonBoss] = &DungeonClearValueContext::next_dungeon_boss;
        creators[DcKey::LiveBoss] = &DungeonClearValueContext::dungeon_clear_live_boss;
        creators[DcKey::Enabled] = &DungeonClearValueContext::dungeon_clear_enabled;
        creators[DcKey::Paused] = &DungeonClearValueContext::dungeon_clear_paused;
        creators[DcKey::PauseReason] = &DungeonClearValueContext::dungeon_clear_pause_reason;
        creators[DcKey::PausedDoor] = &DungeonClearValueContext::dungeon_clear_paused_door;
        creators[DcKey::Skipped] = &DungeonClearValueContext::dungeon_clear_skipped;
        creators[DcKey::ClearedAnchors] = &DungeonClearValueContext::dungeon_clear_cleared_anchors;
        creators[DcKey::SeenBosses] = &DungeonClearValueContext::dungeon_clear_seen_bosses;
        creators[DcKey::SeenDueEvents] = &DungeonClearValueContext::dungeon_clear_seen_due_events;
        creators[DcKey::StickyBoss] = &DungeonClearValueContext::dungeon_clear_sticky_boss;
        creators[DcKey::SelectedBoss] = &DungeonClearValueContext::dungeon_clear_selected_boss;
        creators[DcKey::RunInstance] = &DungeonClearValueContext::dungeon_clear_run_instance;
        creators[DcKey::StallReason] = &DungeonClearValueContext::dungeon_clear_stall_reason;
        creators[DcKey::LastSaidReason] = &DungeonClearValueContext::dungeon_clear_last_said_reason;
        creators[DcKey::Phase] = &DungeonClearValueContext::dungeon_clear_phase;
        creators[DcKey::FallbackTarget] = &DungeonClearValueContext::dungeon_clear_fallback_target;
        creators[DcKey::PartyTank] = &DungeonClearValueContext::dungeon_clear_party_tank;
        creators[DcKey::LongPath] = &DungeonClearValueContext::dungeon_clear_long_path;
        creators[DcKey::CurrentHop] = &DungeonClearValueContext::dungeon_clear_current_hop;
        creators[DcKey::FarTargets] = &DungeonClearValueContext::dungeon_clear_far_targets;
        creators[DcKey::RoomTrashRemaining] = &DungeonClearValueContext::dungeon_clear_room_trash_remaining;
        creators[DcKey::BlockingDoor] = &DungeonClearValueContext::dungeon_clear_blocking_door;
        creators[DcKey::EngageTrashTarget] = &DungeonClearValueContext::dungeon_clear_engage_trash_target;
        creators[DcKey::FollowerState] = &DungeonClearValueContext::dungeon_clear_follower_state;
        creators[DcKey::SwimState] = &DungeonClearValueContext::dungeon_clear_swim_state;
        creators[DcKey::LootSkip] = &DungeonClearValueContext::dungeon_clear_loot_skip;
        creators[DcKey::LootCampGuid] = &DungeonClearValueContext::dungeon_clear_loot_camp_guid;
        creators[DcKey::LootCampStart] = &DungeonClearValueContext::dungeon_clear_loot_camp_start;
        creators[DcKey::FollowedTank] = &DungeonClearValueContext::dungeon_clear_followed_tank;
        creators[DcKey::PullMode] = &DungeonClearValueContext::dungeon_clear_pull_mode;
        creators[DcKey::PullModeCurrent] = &DungeonClearValueContext::dungeon_clear_pull_mode_current;
        creators[DcKey::PullTarget] = &DungeonClearValueContext::dungeon_clear_pull_target;
        creators[DcKey::HealTarget] = &DungeonClearValueContext::dungeon_clear_heal_target;
        creators[DcKey::PullSetting] = &DungeonClearValueContext::dungeon_clear_pull_setting;
        creators[DcKey::PullContext] = &DungeonClearValueContext::dungeon_clear_pull_context;
        creators[DcKey::ApproachState] = &DungeonClearValueContext::dungeon_clear_approach_state;
        creators[DcKey::TickMemo] = &DungeonClearValueContext::dungeon_clear_tick_memo;
        creators[DcKey::EventProgress] = &DungeonClearValueContext::dungeon_clear_event_progress;
        creators[DcKey::ConditionalEventProgress] = &DungeonClearValueContext::dungeon_clear_conditional_event_progress;
    }

private:
    static UntypedValue* dungeon_bosses(PlayerbotAI* ai) { return new DungeonBossesValue(ai); }
    static UntypedValue* next_dungeon_boss(PlayerbotAI* ai) { return new NextDungeonBossValue(ai); }
    static UntypedValue* dungeon_clear_live_boss(PlayerbotAI* ai) { return new DungeonClearLiveBossValue(ai); }
    static UntypedValue* dungeon_clear_enabled(PlayerbotAI* ai) { return new DungeonClearEnabledValue(ai); }
    static UntypedValue* dungeon_clear_paused(PlayerbotAI* ai) { return new DungeonClearPausedValue(ai); }
    static UntypedValue* dungeon_clear_pause_reason(PlayerbotAI* ai) { return new DungeonClearPauseReasonValue(ai); }
    static UntypedValue* dungeon_clear_paused_door(PlayerbotAI* ai) { return new DungeonClearPausedDoorValue(ai); }
    static UntypedValue* dungeon_clear_skipped(PlayerbotAI* ai) { return new DungeonClearSkippedValue(ai); }
    static UntypedValue* dungeon_clear_cleared_anchors(PlayerbotAI* ai) { return new DungeonClearClearedAnchorsValue(ai); }
    static UntypedValue* dungeon_clear_seen_bosses(PlayerbotAI* ai) { return new DungeonClearSeenBossesValue(ai); }
    static UntypedValue* dungeon_clear_seen_due_events(PlayerbotAI* ai) { return new DungeonClearSeenDueEventsValue(ai); }
    static UntypedValue* dungeon_clear_sticky_boss(PlayerbotAI* ai) { return new DungeonClearStickyBossValue(ai); }
    static UntypedValue* dungeon_clear_selected_boss(PlayerbotAI* ai) { return new DungeonClearSelectedBossValue(ai); }
    static UntypedValue* dungeon_clear_run_instance(PlayerbotAI* ai) { return new DungeonClearRunInstanceValue(ai); }
    static UntypedValue* dungeon_clear_stall_reason(PlayerbotAI* ai) { return new DungeonClearStallReasonValue(ai); }
    static UntypedValue* dungeon_clear_last_said_reason(PlayerbotAI* ai) { return new DungeonClearLastSaidReasonValue(ai); }
    static UntypedValue* dungeon_clear_phase(PlayerbotAI* ai) { return new DungeonClearPhaseValue(ai); }
    static UntypedValue* dungeon_clear_fallback_target(PlayerbotAI* ai) { return new DungeonClearFallbackTargetValue(ai); }
    static UntypedValue* dungeon_clear_party_tank(PlayerbotAI* ai) { return new DungeonClearPartyTankValue(ai); }
    static UntypedValue* dungeon_clear_long_path(PlayerbotAI* ai) { return new DungeonClearLongPathValue(ai); }
    static UntypedValue* dungeon_clear_current_hop(PlayerbotAI* ai) { return new DungeonClearCurrentHopValue(ai); }
    static UntypedValue* dungeon_clear_far_targets(PlayerbotAI* ai) { return new DungeonClearFarTargetsValue(ai); }
    static UntypedValue* dungeon_clear_room_trash_remaining(PlayerbotAI* ai) { return new DungeonClearRoomTrashValue(ai); }
    static UntypedValue* dungeon_clear_blocking_door(PlayerbotAI* ai) { return new DungeonClearBlockingDoorValue(ai); }
    static UntypedValue* dungeon_clear_engage_trash_target(PlayerbotAI* ai) { return new DungeonClearEngageTrashTargetValue(ai); }
    static UntypedValue* dungeon_clear_follower_state(PlayerbotAI* ai) { return new DungeonClearFollowerStateValue(ai); }
    static UntypedValue* dungeon_clear_swim_state(PlayerbotAI* ai) { return new DungeonClearSwimStateValue(ai); }
    static UntypedValue* dungeon_clear_loot_skip(PlayerbotAI* ai) { return new DungeonClearLootSkipValue(ai); }
    static UntypedValue* dungeon_clear_loot_camp_guid(PlayerbotAI* ai) { return new DungeonClearLootCampGuidValue(ai); }
    static UntypedValue* dungeon_clear_loot_camp_start(PlayerbotAI* ai) { return new DungeonClearLootCampStartValue(ai); }
    static UntypedValue* dungeon_clear_followed_tank(PlayerbotAI* ai) { return new DungeonClearFollowedTankValue(ai); }
    static UntypedValue* dungeon_clear_pull_mode(PlayerbotAI* ai) { return new DungeonClearPullModeValue(ai); }
    static UntypedValue* dungeon_clear_pull_mode_current(PlayerbotAI* ai) { return new DungeonClearPullModeCurrentValue(ai); }
    static UntypedValue* dungeon_clear_pull_target(PlayerbotAI* ai) { return new DungeonClearPullTargetValue(ai); }
    static UntypedValue* dungeon_clear_heal_target(PlayerbotAI* ai) { return new DungeonClearHealTargetValue(ai); }
    static UntypedValue* dungeon_clear_pull_setting(PlayerbotAI* ai) { return new DungeonClearPullSettingValue(ai); }
    static UntypedValue* dungeon_clear_pull_context(PlayerbotAI* ai) { return new DungeonClearPullContextValue(ai); }
    static UntypedValue* dungeon_clear_approach_state(PlayerbotAI* ai) { return new DungeonClearApproachStateValue(ai); }
    static UntypedValue* dungeon_clear_tick_memo(PlayerbotAI* ai) { return new DungeonClearTickMemoValue(ai); }
    static UntypedValue* dungeon_clear_event_progress(PlayerbotAI* ai) { return new DungeonEventProgressValue(ai); }
    static UntypedValue* dungeon_clear_conditional_event_progress(PlayerbotAI* ai) { return new DungeonConditionalEventProgressValue(ai); }
};

#endif
