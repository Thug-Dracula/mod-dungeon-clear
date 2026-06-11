/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARACTIONCONTEXT_H
#define _PLAYERBOT_DUNGEONCLEARACTIONCONTEXT_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Ai/Dungeon/DungeonClear/Action/BetterLootRollAction.h"
#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Action/DungeonClearChatActions.h"
#include "Ai/Dungeon/DungeonClear/Action/StayDeadAction.h"

class DungeonClearActionContext : public NamedObjectContext<Action>
{
public:
    DungeonClearActionContext()
    {
        creators["dungeon clear advance"] = &DungeonClearActionContext::advance;
        creators["dungeon clear engage trash"] = &DungeonClearActionContext::engage_trash;
        creators["dungeon clear engage boss"] = &DungeonClearActionContext::engage_boss;
        creators["dungeon clear room clear"] = &DungeonClearActionContext::room_clear;
        creators["dungeon clear clear stalled"] = &DungeonClearActionContext::clear_stalled;
        creators["dungeon clear follow tank"] = &DungeonClearActionContext::follow_tank;
        creators["dungeon clear disable on death"] = &DungeonClearActionContext::disable_on_death;
        creators["dungeon clear disable on cleared"] = &DungeonClearActionContext::disable_on_cleared;
        creators["dungeon clear door blocked"] = &DungeonClearActionContext::door_blocked;
        creators["dungeon clear door reopened"] = &DungeonClearActionContext::door_reopened;
        creators["dungeon clear filter loot"] = &DungeonClearActionContext::filter_loot;
        creators["dungeon clear pull"] = &DungeonClearActionContext::pull;
        creators["dungeon clear pull maneuver"] = &DungeonClearActionContext::pull_maneuver;
        creators["dungeon clear hold at camp"] = &DungeonClearActionContext::hold_at_camp;
        creators["dungeon clear stay at camp"] = &DungeonClearActionContext::stay_at_camp;
        creators["dungeon clear assist camp"] = &DungeonClearActionContext::assist_camp;
        creators["dungeon clear assist camp combat"] = &DungeonClearActionContext::assist_camp_combat;
        creators["dungeon clear regroup combat"] = &DungeonClearActionContext::regroup_combat;

        creators["dc on"] = &DungeonClearActionContext::dc_on;
        creators["dc off"] = &DungeonClearActionContext::dc_off;
        creators["dc skip"] = &DungeonClearActionContext::dc_skip;
        creators["dc pause"] = &DungeonClearActionContext::dc_pause;
        creators["dc pull"] = &DungeonClearActionContext::dc_pull;
        creators["dc status"] = &DungeonClearActionContext::dc_status;
        creators["dc bosses"] = &DungeonClearActionContext::dc_bosses;
        creators["dc go"] = &DungeonClearActionContext::dc_go;

        // Override mod-playerbots' "loot roll": a bot in "bot self" mode does
        // not auto-vote on group loot (it shares the human's GUID and would
        // pre-empt their roll), and bots Need/Greed gear above their level
        // that they will wear once they reach it instead of greed/passing.
        // DungeonClearLootRollPendingTrigger additionally drives this action
        // the tick a roll window opens, instead of stock's randomized poll.
        // Server-wide, gated by DungeonClear.BetterLootRolling; inert (defers
        // to stock LootRollAction) when off. Last-registration-wins.
        creators["loot roll"] = &DungeonClearActionContext::better_loot_roll;

        // Override mod-playerbots' "auto release" so dead bots stay dead instead
        // of releasing to the graveyard. Dungeon/raid maps only, gated by
        // DungeonClear.PreventBotRelease; inert (defers to stock) when that flag
        // is off or the bot is outside an instance. Last-registration-wins, so
        // this replaces the playerbots creator for every bot of this class.
        creators["auto release"] = &DungeonClearActionContext::auto_release;
    }

private:
    static Action* advance(PlayerbotAI* ai) { return new DungeonClearAdvanceAction(ai); }
    static Action* engage_trash(PlayerbotAI* ai) { return new DungeonClearEngageTrashAction(ai); }
    static Action* engage_boss(PlayerbotAI* ai) { return new DungeonClearEngageBossAction(ai); }
    static Action* room_clear(PlayerbotAI* ai) { return new DungeonClearRoomClearAction(ai); }
    static Action* clear_stalled(PlayerbotAI* ai) { return new DungeonClearClearStalledAction(ai); }
    static Action* follow_tank(PlayerbotAI* ai) { return new DungeonClearFollowTankAction(ai); }
    static Action* disable_on_death(PlayerbotAI* ai) { return new DungeonClearDisableOnDeathAction(ai); }
    static Action* disable_on_cleared(PlayerbotAI* ai) { return new DungeonClearDisableOnClearedAction(ai); }
    static Action* door_blocked(PlayerbotAI* ai) { return new DungeonClearDoorBlockedAction(ai); }
    static Action* door_reopened(PlayerbotAI* ai) { return new DcResumeOnDoorOpenedAction(ai); }
    static Action* filter_loot(PlayerbotAI* ai) { return new DungeonClearFilterLootAction(ai); }
    static Action* pull(PlayerbotAI* ai) { return new DungeonClearPullAction(ai); }
    static Action* pull_maneuver(PlayerbotAI* ai) { return new DungeonClearPullManeuverAction(ai); }
    static Action* hold_at_camp(PlayerbotAI* ai) { return new DungeonClearHoldAtCampAction(ai); }
    static Action* stay_at_camp(PlayerbotAI* ai) { return new DungeonClearStayAtCampAction(ai); }
    static Action* assist_camp(PlayerbotAI* ai) { return new DungeonClearAssistCampAction(ai); }
    static Action* assist_camp_combat(PlayerbotAI* ai) { return new DungeonClearAssistCampCombatAction(ai); }
    static Action* regroup_combat(PlayerbotAI* ai) { return new DungeonClearRegroupCombatAction(ai); }

    static Action* dc_on(PlayerbotAI* ai) { return new DcOnAction(ai); }
    static Action* dc_off(PlayerbotAI* ai) { return new DcOffAction(ai); }
    static Action* dc_skip(PlayerbotAI* ai) { return new DcSkipAction(ai); }
    static Action* dc_pause(PlayerbotAI* ai) { return new DcPauseAction(ai); }
    static Action* dc_pull(PlayerbotAI* ai) { return new DcPullAction(ai); }
    static Action* dc_status(PlayerbotAI* ai) { return new DcStatusAction(ai); }
    static Action* dc_bosses(PlayerbotAI* ai) { return new DcBossesAction(ai); }
    static Action* dc_go(PlayerbotAI* ai) { return new DcGoAction(ai); }

    static Action* better_loot_roll(PlayerbotAI* ai) { return new DungeonClearBetterLootRollAction(ai); }
    static Action* auto_release(PlayerbotAI* ai) { return new DungeonClearStayDeadAction(ai); }
};

#endif
