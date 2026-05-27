/*
 * mod-dungeon-clear — DungeonClearAddonHook.cpp
 *
 * PlayerScript hook that intercepts addon messages (LANG_ADDON, prefix "DC")
 * sent by the DungeonClear companion addon.  Parses "CMD\t<sub>\t<param>"
 * payloads and dispatches to the same tank-bot actions that the `.dc` slash
 * command uses.
 *
 * The addon sends commands via SendAddonMessage("DC", ..., "PARTY") which
 * arrives as CHAT_MSG_PARTY / LANG_ADDON.  Our OnPlayerBeforeSendChatMessage
 * hook fires before the ChatHandler switch statement, parses the command,
 * dispatches it silently (DoSpecificAction with silent=true), then consumes
 * the message so no further chat processing occurs.
 */

#include "ScriptMgr.h"
#include "PlayerScript.h"
#include "Chat.h"
#include "Log.h"
#include "Player.h"
#include "ServerFacade.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "DungeonClearDispatch.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"

namespace
{
    // Send an error back to the player via the addon message channel.
    void SendAddonError(Player* player, std::string const& msg)
    {
        if (!player)
            return;

        std::string const payload = "DC\tERROR\t" + msg;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, payload.c_str(),
                                     LANG_ADDON, CHAT_TAG_NONE,
                                     player->GetGUID(), player->GetName());

        ServerFacade::instance().SendPacket(player, &data);
    }
}

class DungeonClearAddonHookScript : public PlayerScript
{
public:
    DungeonClearAddonHookScript()
        : PlayerScript("DungeonClearAddonHookScript", {
            PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE
        }) {}

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        // Only intercept addon messages.
        if (lang != LANG_ADDON)
            return;

        // Accept PARTY and PARTY_LEADER (standard addon channel).
        if (type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER)
            return;

        // Addon messages arrive as "DC\tCMD\t<sub>[\t<param>]".
        if (msg.size() < 6 || msg.substr(0, 3) != "DC\t")
            return;

        // Strip the "DC\t" prefix to get the inner payload.
        std::string const inner = msg.substr(3);

        // Only handle CMD messages; STATUS/BOSS/CHAT are server→client only.
        if (inner.size() < 4 || inner.substr(0, 4) != "CMD\t")
            return;

        // Parse "CMD\t<subcommand>[\t<param>]"
        std::string const cmdPayload = inner.substr(4);
        std::string subCmd;
        std::string param;

        auto const tabPos = cmdPayload.find('\t');
        if (tabPos == std::string::npos)
        {
            subCmd = cmdPayload;
        }
        else
        {
            subCmd = cmdPayload.substr(0, tabPos);
            param = cmdPayload.substr(tabPos + 1);
        }

        if (subCmd.empty())
            return;

        // Map subcommand strings to action names.
        std::string action;
        if (subCmd == "on")         action = "dc on";
        else if (subCmd == "off")   action = "dc off";
        else if (subCmd == "skip")  action = "dc skip";
        else if (subCmd == "status") action = "dc status";
        else if (subCmd == "bosses") action = "dc bosses";
        else if (subCmd == "go")    action = "dc go";
        else
        {
            LOG_DEBUG("module", "mod-dungeon-clear: unknown addon subcommand '{}' from {}",
                      subCmd, player->GetName());
            return;
        }

        // Dispatch to the tank bot(s) silently (no PlaySound emotes).
        if (!DungeonClearDispatch::DispatchToTankBots(player, action, param))
            SendAddonError(player, "No tank bot found in your group.");

        // Consume the message so the ChatHandler switch doesn't process it
        // as a real party chat message.
        msg.clear();
        type = CHAT_MSG_ADDON;
    }
};

void AddSC_dungeon_clear_addon_hook()
{
    new DungeonClearAddonHookScript();
}
