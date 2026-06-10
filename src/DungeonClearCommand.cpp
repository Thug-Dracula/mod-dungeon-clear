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
#include "StringFormat.h"

#include <cmath>

#include "DungeonClearDispatch.h"
#include "Util/DcSpectator.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettingsRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"

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

    // Format a resolved raw double per the registry type, so the printout reads
    // the way the conf line is written (true/false, ints, floats).
    std::string FormatDcValue(DcSettingDef const& d, double raw)
    {
        switch (d.type)
        {
            case DcType::Bool:
                return raw != 0.0 ? "true" : "false";
            case DcType::UInt:
            case DcType::Int:
                return Acore::StringFormat("{}", static_cast<int64>(std::lround(raw)));
            case DcType::Float:
            default:
                return Acore::StringFormat("{:.2f}", raw);
        }
    }

    // Dumps every DungeonClear tunable as the module actually reads it: the live
    // conf/default value, plus the per-run effective value when the issuer's run
    // has an addon override active. This is a pure read of sConfigMgr through the
    // DcSettings accessor, so it reflects exactly what the AI sees this tick —
    // use it to confirm whether a conf edit took effect (no `.reload config`).
    bool HandleConfig(ChatHandler* handler)
    {
        Player* issuer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!issuer)
        {
            handler->SendSysMessage("This command must be used in-game.");
            return true;
        }

        // Resolve the run owner (leader tank) so we can surface per-run overrides.
        // Empty when the issuer isn't in a DC run — then only conf/defaults show.
        Player* leader = DcLeaderSignal::FindLeaderTank(issuer);
        ObjectGuid const runOwner = leader ? leader->GetGUID() : ObjectGuid::Empty;

        handler->SendSysMessage("DungeonClear config (effective values; * = addon override):");
        for (DcSettingDef const& d : kDcSettings)
        {
            double const confVal = DcSettings::GetEffectiveRaw(ObjectGuid::Empty, d);
            double const effVal  = DcSettings::GetEffectiveRaw(runOwner, d);
            bool const overridden =
                !runOwner.IsEmpty() && DcSettings::HasOverride(runOwner, d.key);

            std::string line;
            if (overridden)
                line = Acore::StringFormat("  * DungeonClear.{} = {} (conf {})",
                                           d.key, FormatDcValue(d, effVal),
                                           FormatDcValue(d, confVal));
            else
                line = Acore::StringFormat("    DungeonClear.{} = {}",
                                           d.key, FormatDcValue(d, confVal));
            handler->SendSysMessage(line);
        }
        return true;
    }

    // Spectator free-camera toggle. Acts on the ISSUER directly (session
    // plumbing, not bot behavior) — it must NOT go through DispatchToTankBots
    // or the action pipeline: the issuer may not even be the tank, and the
    // possession belongs to their session alone. See Util/DcSpectator.h.
    bool HandleSpectate(ChatHandler* handler)
    {
        Player* issuer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!issuer)
        {
            handler->SendSysMessage("This command must be used in-game.");
            return true;
        }

        std::string whyNot;
        if (!DcSpectator::Toggle(issuer, &whyNot))
            handler->SendSysMessage(whyNot);
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
            { "config", HandleConfig, SEC_PLAYER, Console::No },
            { "spectate", HandleSpectate, SEC_PLAYER, Console::No },
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
