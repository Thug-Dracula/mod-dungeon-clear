/*
 * mod-dungeon-clear — DungeonClearCommand.cpp
 *
 * Slash command `.dc on|off|skip|status|bosses`. A convenience entry point that
 * works with zero config (unlike the chat keywords, which need the
 * "dungeon clear" strategy applied — see DungeonClearModule.cpp).
 *
 * Each subcommand dispatches the matching DungeonClear action ("dc on", …) to
 * the issuing player's tank bot(s) via PlayerbotAI::DoSpecificAction. The
 * actions already self-authorize (owner must be a real player in the bot's
 * group) and self-gate (e.g. `dc on` is tank-only), so we carry the issuing
 * player as the Event owner and let the existing action logic decide.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Group.h"
#include "Player.h"

#include "DungeonClearDispatch.h"

using namespace Acore::ChatCommands;

namespace
{
    bool RunDcCommand(ChatHandler* handler, std::string const& action, std::string const& param = "")
    {
        Player* issuer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!issuer)
        {
            handler->SendSysMessage("This command must be used in-game.");
            return true;
        }

        if (!DungeonClearDispatch::DispatchToTankBots(issuer, action, param))
            handler->SendSysMessage("No tank bot found in your group.");

        return true;
    }
}

class dungeon_clear_command_script : public CommandScript
{
public:
    dungeon_clear_command_script() : CommandScript("dungeon_clear_command_script") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable dcTable =
        {
            { "on",     HandleOn,     SEC_PLAYER, Console::No },
            { "off",    HandleOff,    SEC_PLAYER, Console::No },
            { "skip",   HandleSkip,   SEC_PLAYER, Console::No },
            { "pause",  HandlePause,  SEC_PLAYER, Console::No },
            { "pull",   HandlePull,   SEC_PLAYER, Console::No },
            { "status", HandleStatus, SEC_PLAYER, Console::No },
            { "bosses", HandleBosses, SEC_PLAYER, Console::No },
            { "go",     HandleGo,     SEC_PLAYER, Console::No },
        };
        static ChatCommandTable root = { { "dc", dcTable } };
        return root;
    }

    static bool HandleOn(ChatHandler* handler)     { return RunDcCommand(handler, "dc on"); }
    static bool HandleOff(ChatHandler* handler)    { return RunDcCommand(handler, "dc off"); }
    static bool HandleSkip(ChatHandler* handler)   { return RunDcCommand(handler, "dc skip"); }
    static bool HandlePause(ChatHandler* handler)  { return RunDcCommand(handler, "dc pause"); }
    static bool HandlePull(ChatHandler* handler, Optional<std::string> param) { return RunDcCommand(handler, "dc pull", param ? *param : ""); }
    static bool HandleStatus(ChatHandler* handler, Optional<std::string> param) { return RunDcCommand(handler, "dc status", param ? *param : ""); }
    static bool HandleBosses(ChatHandler* handler, Optional<std::string> param) { return RunDcCommand(handler, "dc bosses", param ? *param : ""); }
    static bool HandleGo(ChatHandler* handler, Tail targetBoss) { return RunDcCommand(handler, "dc go", std::string(targetBoss)); }
};

void AddSC_dungeon_clear_command()
{
    new dungeon_clear_command_script();
}
