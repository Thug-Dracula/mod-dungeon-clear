/*
 * mod-dungeon-clear — DungeonClearDispatch.h
 *
 * Shared helper that dispatches DungeonClear subcommands to a player's tank
 * bot(s). Used by both the `.dc` chat command (DungeonClearCommand.cpp) and
 * the addon-message hook (DungeonClearAddonHook.cpp).
 */

#ifndef _DUNGEON_CLEAR_DISPATCH_H
#define _DUNGEON_CLEAR_DISPATCH_H

#include <string>
#include "Event.h"
#include "Group.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"

namespace DungeonClearDispatch
{
    // Dispatch "dc <sub>" to every tank bot in the issuer's group. Returns the
    // number of bots that handled it; 0 means "no tank bot found".
    inline uint32 DispatchToTankBots(Player* issuer, std::string const& action, std::string const& param = "")
    {
        if (!issuer)
            return 0;

        Group* group = issuer->GetGroup();
        if (!group)
            return 0;

        uint32 dispatched = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
            if (!botAI)
                continue;
            if (!PlayerbotAI::IsTank(member))
                continue;

            botAI->DoSpecificAction(action, Event("dc", param, issuer), true);
            ++dispatched;
        }
        return dispatched;
    }
}

#endif
