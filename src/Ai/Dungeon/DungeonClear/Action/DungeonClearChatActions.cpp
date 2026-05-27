/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearChatActions.h"

#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "InstanceScript.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Playerbots.h"

namespace
{
    bool IsAuthorized(Player* bot, Event const& event)
    {
        Player* owner = const_cast<Event&>(event).getOwner();
        if (!owner)
            return false;
        // Reject true bots, but allow self-bot players (a real player whose
        // own PlayerbotAI has master == bot). Without this exception the
        // moment a player enables bot self mode they would be silently
        // locked out of every dc command.
        if (PlayerbotAI* ownerAI = GET_PLAYERBOT_AI(owner))
            if (!ownerAI->IsRealPlayer())
                return false;
        if (!bot || !bot->GetGroup())
            return false;
        return bot->GetGroup()->IsMember(owner->GetGUID());
    }

    bool AnyPartyMemberDead(Player* bot)
    {
        if (!bot)
            return false;
        if (bot->isDead())
            return true;
        Group* group = bot->GetGroup();
        if (!group)
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot)
                continue;
            if (member->GetMapId() != bot->GetMapId())
                continue;
            if (member->isDead())
                return true;
        }
        return false;
    }

    std::string FirstDeadName(Player* bot)
    {
        if (!bot)
            return "Someone";
        if (bot->isDead())
            return bot->GetName();
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot)
                    continue;
                if (member->GetMapId() != bot->GetMapId())
                    continue;
                if (member->isDead())
                    return member->GetName();
            }
        }
        return "Someone";
    }
}

bool DcOnAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to enable dungeon clear");
        return false;
    }
    // Party chat fans `dc on` out to every bot the master owns, so non-tank
    // followers land here too. They don't lead the clear, but they must follow
    // the tank while it runs. That follow-tank behavior lives in the "dungeon
    // clear" strategy, applied to every bot at login; re-assert it here as a
    // safety net (e.g. a bot that logged in before contexts registered), then
    // stay quiet — only the tank announces. The follower's follow-tank trigger
    // self-activates off the tank's enabled flag via DungeonClearPartyTankValue,
    // so nothing else is needed for them.
    if (!PlayerbotAI::IsTank(bot))
    {
        if (!botAI->HasStrategy("dungeon clear", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("+dungeon clear", BOT_STATE_NON_COMBAT);
        return true;
    }
    if (!bot->GetMap() || !bot->GetMap()->IsDungeon())
    {
        botAI->TellError("Not in a dungeon.");
        return false;
    }

    auto const& bosses = AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");
    if (bosses.empty())
    {
        botAI->TellError("No bosses found for this map.");
        return false;
    }

    if (AnyPartyMemberDead(bot))
    {
        botAI->TellError(FirstDeadName(bot) + " is dead — rez and try again.");
        return false;
    }

    // Activate the "dungeon clear" strategy on the non-combat engine. This
    // is what installs the advance/engage/stall trigger ladder that actually
    // drives movement and logging; without it, flipping "enabled" below does
    // nothing (the triggers aren't on any engine to fire). The stock
    // playerbots tree no longer adds it via AiFactory — it must be applied
    // here so both input paths (chat keyword and `.dc` command) work.
    if (!botAI->HasStrategy("dungeon clear", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+dungeon clear", BOT_STATE_NON_COMBAT);

    // Reset transient state and enable.
    context->GetValue<bool>("dungeon clear enabled")->Set(true);
    context->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
    context->GetValue<std::unordered_set<uint32>&>("dungeon clear skipped")->Get().clear();
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<uint32>("dungeon clear last target entry")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    // Force a fresh long-path build on the first Advance tick. Without
    // this, a stale path from a previous `dc on`/`dc off` cycle would
    // be reused.
    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    std::string const target = next.has_value() ? next->name : "the next boss";
    DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tDungeon clear enabled. Heading to " + target + ".");

    // Trigger instant status addon message update
    botAI->DoSpecificAction("dc status", event, true);
    return true;
}

bool DcOffAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to disable dungeon clear");
        return false;
    }

    // Non-tank followers have nothing to disable: their follow-tank trigger
    // deactivates on its own the instant the tank clears its enabled flag
    // (DungeonClearPartyTankValue then returns null), so they revert to
    // following the player automatically. Leave the inert strategy installed
    // and stay quiet — they must NOT strip it, or they'd stop following the
    // tank on a subsequent `dc on`.
    if (!PlayerbotAI::IsTank(bot))
        return true;

    context->GetValue<bool>("dungeon clear enabled")->Set(false);
    context->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    // Cancel any in-flight advance/engage MoveTo so the bot actually stops
    // walking when the player says `dc off`. Without this, a previously
    // queued MOVEMENT_NORMAL/MOVEMENT_COMBAT spline keeps running to its
    // endpoint even though the strategy is now disabled — the bot looks
    // like it's ignoring the off command for several seconds.
    if (bot)
    {
        if (bot->isMoving())
            bot->StopMoving();
        if (MotionMaster* mm = bot->GetMotionMaster())
            mm->Clear();
    }

    // Leave the "dungeon clear" strategy installed. With the enabled flag now
    // false, every trigger and the multiplier in it are inert, so the tank is
    // already back to stock non-combat behavior — and this matches the
    // auto-disable paths (death / all-cleared), which only flip the flag too.
    // It stays resident on every bot via the login script regardless.

    DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tDungeon clear disabled.");

    // Trigger instant status addon message update
    botAI->DoSpecificAction("dc status", event, true);
    return true;
}

bool DcSkipAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to skip");
        return false;
    }
    if (!AI_VALUE(bool, "dungeon clear enabled"))
    {
        botAI->TellError("Dungeon clear is not enabled.");
        return false;
    }

    std::optional<DungeonBossInfo> current = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!current.has_value())
    {
        botAI->TellError("No current boss to skip.");
        return false;
    }

    std::unordered_set<uint32>& skipped =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear skipped")->Get();
    skipped.insert(current->entry);

    // New target gets a clean slate.
    context->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<uint32>("dungeon clear last target entry")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    // Invalidate the long-path cache so the next Advance tick rebuilds
    // for the new target boss.
    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    // Force the cached next-boss value to recompute on next call.
    context->GetValue<std::optional<DungeonBossInfo>>("next dungeon boss")->Reset();
    std::optional<DungeonBossInfo> after = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");

    if (after.has_value())
    {
        DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tSkipped " + current->name + ". Heading to " + after->name + ".");
    }
    else
    {
        DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tSkipped " + current->name + ". No bosses left \xe2\x80\x94 disabling.");
        context->GetValue<bool>("dungeon clear enabled")->Set(false);
    }

    // Trigger instant status addon message update
    botAI->DoSpecificAction("dc status", event, true);
    return true;
}

bool DcStatusAction::Execute(Event event)
{
    bool const enabled = AI_VALUE(bool, "dungeon clear enabled");
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    auto const& skipped = AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped");
    std::string const& stall = AI_VALUE(std::string&, "dungeon clear stall reason");

    std::string const param = event.getParam();
    bool const silent = (param == "addon" || param == "silent");

    if (!silent)
    {
        std::ostringstream msg;
        msg << "Dungeon clear: " << (enabled ? "on" : "off")
            << ". Next boss: " << (next.has_value() ? next->name : "none")
            << ". Skipped: " << skipped.size() << ".";
        if (!stall.empty())
            msg << " Stalled: " << stall;
        DungeonClearUtil::SendAddonMessage(botAI, "CHAT\t" + msg.str());
    }

    // Calculate dynamic state for addon UI
    std::string stateStr = "off";
    if (enabled)
    {
        if (bot->IsInCombat())
        {
            Unit* currentTarget = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            if (currentTarget && next.has_value() && currentTarget->GetEntry() == next->entry)
                stateStr = "fighting_boss";
            else
                stateStr = "fighting_trash";
        }
        else if (!stall.empty())
        {
            ObjectGuid door = botAI->GetAiObjectContext()->GetValue<ObjectGuid>("dungeon clear blocking door")->Get();
            if (!door.IsEmpty())
                stateStr = "door_blocked";
            else
                stateStr = "stalled";
        }
        else if (AI_VALUE(bool, "has available loot") || AI_VALUE(bool, "can loot") ||
                 DungeonClearUtil::IsAnyPartyMemberLooting(bot))
        {
            stateStr = "looting";
        }
        else if (!DungeonClearUtil::IsPartyReady(bot, 90.0f, 75.0f, 40.0f))
        {
            stateStr = "resting";
        }
        else if (bot->isMoving())
        {
            stateStr = "moving";
        }
        else
        {
            stateStr = "idle";
        }
    }

    std::ostringstream addonMsg;
    addonMsg << "STATUS\t"
             << (enabled ? "1" : "0") << "\t"
             << (next.has_value() ? std::to_string(next->entry) : "0") << "\t"
             << (next.has_value() ? next->name : "None") << "\t"
             << (stall.empty() ? "" : stall) << "\t"
             << skipped.size() << "\t"
             << stateStr;

    DungeonClearUtil::SendAddonMessage(botAI, addonMsg.str());
    return true;
}

bool DcBossesAction::Execute(Event event)
{
    auto const& bosses = AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");

    std::string const param = event.getParam();
    bool const silent = (param == "addon" || param == "silent");

    DungeonClearUtil::SendAddonMessage(botAI, "BOSS_START");

    if (bosses.empty())
    {
        if (!silent)
            DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tNo bosses found for this map.");
        DungeonClearUtil::SendAddonMessage(botAI, "BOSS_END");
        return true;
    }

    auto const& skipped = AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped");
    for (DungeonBossInfo const& info : bosses)
    {
        std::string statusStr = "dead";
        if (skipped.count(info.entry))
            statusStr = "skipped";
        else if (!DungeonClearUtil::IsCreaturePresentOnMap(bot, info.entry))
            statusStr = "missing";
        else if (DungeonClearUtil::FindLiveCreatureOnMap(bot, info.entry))
            statusStr = "alive";

        std::ostringstream addonMsg;
        addonMsg << "BOSS\t"
                 << info.entry << "\t"
                 << info.encounterIndex << "\t"
                 << info.name << "\t"
                 << statusStr << "\t"
                 << static_cast<int>(info.x) << "\t"
                 << static_cast<int>(info.y) << "\t"
                 << static_cast<int>(info.z);

        DungeonClearUtil::SendAddonMessage(botAI, addonMsg.str());

        if (!silent)
        {
            std::ostringstream line;
            line << info.encounterIndex << ". " << info.name
                 << " @ (" << static_cast<int>(info.x) << ", "
                 << static_cast<int>(info.y) << ", "
                 << static_cast<int>(info.z) << ") [" << statusStr << "]";

            DungeonClearUtil::SendAddonMessage(botAI, "CHAT\t" + line.str());
        }
    }

    DungeonClearUtil::SendAddonMessage(botAI, "BOSS_END");
    return true;
}

bool DcGoAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to run go command");
        return false;
    }
    if (!AI_VALUE(bool, "dungeon clear enabled"))
    {
        botAI->TellError("Dungeon clear is not enabled. Please enable it first.");
        return false;
    }

    std::string const param = event.getParam();
    if (param.empty())
    {
        botAI->TellError("Usage: .dc go <boss name or entry>");
        return false;
    }

    auto const& bosses = AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");
    DungeonBossInfo const* matched = nullptr;

    bool isNumeric = !param.empty() && std::all_of(param.begin(), param.end(), ::isdigit);
    if (isNumeric)
    {
        uint32 entry = std::stoul(param);
        for (auto const& info : bosses)
        {
            if (info.entry == entry)
            {
                matched = &info;
                break;
            }
        }
    }

    if (!matched)
    {
        std::string query = param;
        std::transform(query.begin(), query.end(), query.begin(), ::tolower);
        for (auto const& info : bosses)
        {
            std::string name = info.name;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name.find(query) != std::string::npos)
            {
                matched = &info;
                break;
            }
        }
    }

    if (!matched)
    {
        botAI->TellError("Could not find boss matching: " + param);
        return false;
    }

    InstanceScript* inst = DungeonClearUtil::GetInstanceScript(bot);
    uint32 const completedMask = inst ? inst->GetCompletedEncounterMask() : 0u;
    if (matched->encounterIndex < 32 && (completedMask & (1u << matched->encounterIndex)))
    {
        botAI->TellError("Boss " + matched->name + " is already dead (encounter complete).");
        return false;
    }

    Map* map = bot->GetMap();
    bool alive = true;
    bool present = false;
    if (map)
    {
        for (auto const& kv : map->GetCreatureBySpawnIdStore())
        {
            Creature* c = kv.second;
            if (c && c->GetEntry() == matched->entry)
            {
                present = true;
                if (!c->IsAlive())
                    alive = false;
            }
        }
    }
    if (present && !alive)
    {
        botAI->TellError("Boss " + matched->name + " is dead.");
        return false;
    }

    std::unordered_set<uint32>& skipped =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear skipped")->Get();
    skipped.erase(matched->entry);

    context->GetValue<uint32>("dungeon clear selected boss")->Set(matched->entry);

    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<uint32>("dungeon clear last target entry")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);

    context->GetValue<std::optional<DungeonBossInfo>>("next dungeon boss")->Reset();

    if (bot->isMoving())
        bot->StopMoving();
    if (MotionMaster* mm = bot->GetMotionMaster())
        mm->Clear();

    DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tTargeting boss: " + matched->name + ". Navigating...");

    botAI->DoSpecificAction("dc status", event, true);
    return true;
}

