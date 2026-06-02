/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARACTIONCONTEXT_H
#define _PLAYERBOT_DUNGEONCLEARACTIONCONTEXT_H

#include "Action.h"
#include "NamedObjectContext.h"
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
        creators["dungeon clear clear stalled"] = &DungeonClearActionContext::clear_stalled;
        creators["dungeon clear follow tank"] = &DungeonClearActionContext::follow_tank;
        creators["dungeon clear disable on death"] = &DungeonClearActionContext::disable_on_death;
        creators["dungeon clear disable on cleared"] = &DungeonClearActionContext::disable_on_cleared;
        creators["dungeon clear door blocked"] = &DungeonClearActionContext::door_blocked;

        creators["dc on"] = &DungeonClearActionContext::dc_on;
        creators["dc off"] = &DungeonClearActionContext::dc_off;
        creators["dc skip"] = &DungeonClearActionContext::dc_skip;
        creators["dc pause"] = &DungeonClearActionContext::dc_pause;
        creators["dc status"] = &DungeonClearActionContext::dc_status;
        creators["dc bosses"] = &DungeonClearActionContext::dc_bosses;
        creators["dc go"] = &DungeonClearActionContext::dc_go;

        // Override mod-playerbots' "auto release" so dead bots stay dead instead
        // of releasing to the graveyard. Gated by DungeonClear.PreventBotRelease;
        // inert (defers to stock) when that flag is off. Last-registration-wins,
        // so this replaces the playerbots creator for every bot of this class.
        creators["auto release"] = &DungeonClearActionContext::auto_release;
    }

private:
    static Action* advance(PlayerbotAI* ai) { return new DungeonClearAdvanceAction(ai); }
    static Action* engage_trash(PlayerbotAI* ai) { return new DungeonClearEngageTrashAction(ai); }
    static Action* engage_boss(PlayerbotAI* ai) { return new DungeonClearEngageBossAction(ai); }
    static Action* clear_stalled(PlayerbotAI* ai) { return new DungeonClearClearStalledAction(ai); }
    static Action* follow_tank(PlayerbotAI* ai) { return new DungeonClearFollowTankAction(ai); }
    static Action* disable_on_death(PlayerbotAI* ai) { return new DungeonClearDisableOnDeathAction(ai); }
    static Action* disable_on_cleared(PlayerbotAI* ai) { return new DungeonClearDisableOnClearedAction(ai); }
    static Action* door_blocked(PlayerbotAI* ai) { return new DungeonClearDoorBlockedAction(ai); }

    static Action* dc_on(PlayerbotAI* ai) { return new DcOnAction(ai); }
    static Action* dc_off(PlayerbotAI* ai) { return new DcOffAction(ai); }
    static Action* dc_skip(PlayerbotAI* ai) { return new DcSkipAction(ai); }
    static Action* dc_pause(PlayerbotAI* ai) { return new DcPauseAction(ai); }
    static Action* dc_status(PlayerbotAI* ai) { return new DcStatusAction(ai); }
    static Action* dc_bosses(PlayerbotAI* ai) { return new DcBossesAction(ai); }
    static Action* dc_go(PlayerbotAI* ai) { return new DcGoAction(ai); }

    static Action* auto_release(PlayerbotAI* ai) { return new DungeonClearStayDeadAction(ai); }
};

#endif
