/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearChatActions.h"

#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "InstanceScript.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
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

    // Forcefully halt the tank's in-flight DC navigation when DC is turned off,
    // paused, or retargeted. The Advance/DoorBlocked glide is an escort spline
    // (MoveSplinePath -> EscortMovementGenerator at MOTION_SLOT_ACTIVE), and
    // stopping it is harder than it looks:
    //   - MotionMaster::Clear() removes the generator, but
    //     EscortMovementGenerator::DoFinalize() only clears unit states — it
    //     never stops the launched movespline, so the server keeps gliding the
    //     bot to the spline's endpoint.
    //   - Unit::StopMoving() early-returns once movespline->Finalized(), so it is
    //     NOT a reliable way to cancel a spline that is mid-flight or between
    //     windows.
    // StopMovingOnCurrentPos() is the reliable primitive: it DisableSpline()s and
    // launches a zero-length spline at the current position, forcibly overriding
    // whatever was playing. Clear() first so the escort generator can't recalc /
    // relaunch, then the override stops the unit dead. Stock follow reclaims the
    // slot on the next tick.
    void HaltAllMovement(Player* bot)
    {
        if (!bot)
            return;
        if (MotionMaster* mm = bot->GetMotionMaster())
            mm->Clear();
        bot->StopMovingOnCurrentPos();
    }
}

bool DcOnAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to enable dungeon clear");
        return false;
    }
    // Party chat fans `dc on` out to every bot the master owns, so non-leader
    // bots land here too — non-tanks AND, in a raid, non-leader (off-)tanks.
    // They don't lead the clear, but they must follow the leader while it runs.
    // That follow-tank behavior lives in the "dungeon clear" strategy, applied
    // to every bot at login; re-assert it here as a safety net (e.g. a bot that
    // logged in before contexts registered), then stay quiet — only the leader
    // announces. The follower's follow-tank trigger self-activates off the
    // leader's enabled flag via DungeonClearPartyTankValue, so nothing else is
    // needed for them. Leadership is the lowest-GUID tank bot in the group, so
    // exactly one bot takes the path below even with several tanks present.
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
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
    // Register this tank with the event-driven status pusher so the server
    // begins emitting STATUS packets on state transitions (replacing the
    // addon's old 2s poll). The matching UnmarkActiveTank lives in every
    // disable path: dc off, skip-to-empty, and DisableDungeonClear.
    DungeonClearUtil::MarkActiveTank(bot->GetGUID());
    context->GetValue<bool>("dungeon clear paused")->Set(false);
    context->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
    context->GetValue<std::unordered_set<uint32>&>("dungeon clear skipped")->Get().clear();
    context->GetValue<std::unordered_set<uint32>&>("dungeon clear seen bosses")->Get().clear();
    context->GetValue<std::map<ObjectGuid, uint32>&>("dungeon clear loot skip")->Get().clear();
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<uint32>("dungeon clear last target entry")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear phase")->Get().clear();
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

    // Non-leader followers have nothing to disable: their follow-tank trigger
    // deactivates on its own the instant the leader clears its enabled flag
    // (DungeonClearPartyTankValue then returns null), so they revert to
    // following the player automatically. Leave the inert strategy installed
    // and stay quiet — they must NOT strip it, or they'd stop following the
    // leader on a subsequent `dc on`.
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
        return true;

    context->GetValue<bool>("dungeon clear enabled")->Set(false);
    DungeonClearUtil::UnmarkActiveTank(bot->GetGUID());
    context->GetValue<bool>("dungeon clear paused")->Set(false);
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

    // Cancel any in-flight advance/engage spline so the bot actually stops
    // walking when the player says `dc off`. Without this, a previously
    // queued spline keeps running to its endpoint even though the strategy is
    // now disabled — the bot looks like it's ignoring the off command for
    // several seconds. See HaltAllMovement for why this can't gate on isMoving().
    HaltAllMovement(bot);

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
    // Only the leader owns the run state. Non-leaders reached via the party-chat
    // keyword fan-out stay quiet (the command/addon path already targets only
    // the leader). Without this they'd each error "not enabled".
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
        return true;
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
        DungeonClearUtil::UnmarkActiveTank(bot->GetGUID());
    }

    // Trigger instant status addon message update
    botAI->DoSpecificAction("dc status", event, true);
    return true;
}

bool DcStatusAction::Execute(Event event)
{
    // Only the leader reports — both the command/addon dispatch and the internal
    // refresh calls (from dc on/off/skip/go/pause) run on the leader, while the
    // party-chat keyword fans out to every bot. Without this gate a raid's
    // non-leader tanks would each emit a duplicate STATUS line to the addon.
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
        return true;

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

    // The STATUS payload itself is built by the shared helper so the on-demand
    // path here and the event-driven change-detector (TickStatusPushes) emit
    // byte-identical strings and can never drift. PushStatus also refreshes the
    // detector's "last pushed" snapshot for this tank, so an explicit `dc
    // status` won't trigger a redundant push on the following tick.
    DungeonClearUtil::PushStatus(botAI);
    return true;
}

bool DcBossesAction::Execute(Event event)
{
    // Only the leader answers the boss-list request — the addon tracks the
    // leader, and the chat-keyword fan-out would otherwise have every raid tank
    // reply with a duplicate list.
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
        return true;

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
    auto& seen = AI_VALUE(std::unordered_set<uint32>&, "dungeon clear seen bosses");

    // The completed-encounter mask is the authoritative "the group killed it"
    // signal: Map::UpdateEncounterState (driven from KillRewarder) flips bit
    // (1 << DungeonEncounter.dbc index), and BossSpawnIndex stores that exact
    // index in encounterIndex, so the bit lines up directly. This is the same
    // kill record NextDungeonBossValue uses to advance the clear — see the long
    // note there on why GetCompletedEncounterMask is correct and GetBossState is
    // not. Reading kill state from the mask (and not from corpse presence) is the
    // crux of the fix: a boss corpse despawns about a minute after death, so the
    // old "present-but-no-live-creature == dead" heuristic made every killed boss
    // silently flip from "dead" to "missing" the moment its body vanished.
    InstanceScript* inst = DungeonClearUtil::GetInstanceScript(bot);
    uint32 const completedMask = inst ? inst->GetCompletedEncounterMask() : 0u;

    for (DungeonBossInfo const& info : bosses)
    {
        bool const killed =
            info.encounterIndex < 32 && (completedMask & (1u << info.encounterIndex)) != 0u;
        bool const liveOnMap = DungeonClearUtil::FindLiveCreatureOnMap(bot, info.entry) != nullptr;
        // Only scan a second time for a corpse when there's no live instance.
        bool const corpseOnMap =
            !liveOnMap && DungeonClearUtil::IsCreaturePresentOnMap(bot, info.entry);

        // Remember every boss we've actually seen alive this run so a later
        // disappearance reads as "missing" rather than an unreached boss whose
        // grid simply hasn't loaded yet.
        if (liveOnMap)
            seen.insert(info.entry);

        // alive   — in zone and the group has not killed it
        // dead    — the group has killed it (kill mask, or a corpse on the floor
        //           for the brief window before the mask flips)
        // missing — was seen alive earlier, is now gone, and was not killed
        // skipped — user/AI chose to pass it by (only while it's still alive)
        std::string statusStr;
        if (killed || corpseOnMap)
            statusStr = "dead";
        else if (skipped.count(info.entry))
            statusStr = "skipped";
        else if (liveOnMap)
            statusStr = "alive";
        else if (seen.count(info.entry))
            statusStr = "missing";
        else
            statusStr = "alive";  // exists in the instance, grid not yet loaded

        // Wing label for split maps (empty for single-wing dungeons). On
        // connected maps like Maraudon this is the only place the wing surfaces
        // — the boss list still holds every boss, it's just annotated by region.
        std::string const wing = DungeonWingRegistry::WingName(bot->GetMapId(), info.entry);

        // Wing is appended as a trailing field; older addons that read only the
        // first seven fields ignore it, so the protocol stays compatible.
        std::ostringstream addonMsg;
        addonMsg << "BOSS\t"
                 << info.entry << "\t"
                 << info.encounterIndex << "\t"
                 << info.name << "\t"
                 << statusStr << "\t"
                 << static_cast<int>(info.x) << "\t"
                 << static_cast<int>(info.y) << "\t"
                 << static_cast<int>(info.z) << "\t"
                 << wing;

        DungeonClearUtil::SendAddonMessage(botAI, addonMsg.str());

        if (!silent)
        {
            std::ostringstream line;
            line << info.encounterIndex << ". " << info.name;
            if (!wing.empty())
                line << " [" << wing << "]";
            line << " @ (" << static_cast<int>(info.x) << ", "
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
    // Leader owns the run; non-leaders reached via the chat-keyword fan-out
    // stay quiet (the command/addon path already targets only the leader).
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
        return true;
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

    // Explicitly targeting a boss is a "go now" intent — clear any pause so the
    // tank actually starts moving toward it instead of holding.
    context->GetValue<bool>("dungeon clear paused")->Set(false);

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

    // Kill any parked escort glide before building the route to the new target
    // (see HaltAllMovement) so the tank doesn't coast down the old path first.
    HaltAllMovement(bot);

    DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tTargeting boss: " + matched->name + ". Navigating...");

    botAI->DoSpecificAction("dc status", event, true);
    return true;
}

bool DcPauseAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to pause dungeon clear");
        return false;
    }

    // Followers have nothing to toggle: their follow-tank trigger reacts to the
    // leader's paused flag via DungeonClearPartyTankValue. Stay quiet.
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
        return true;

    if (!AI_VALUE(bool, "dungeon clear enabled"))
    {
        botAI->TellError("Dungeon clear is not enabled.");
        return false;
    }

    bool const paused = AI_VALUE(bool, "dungeon clear paused");

    if (!paused)
    {
        // Pause: hold in place. The driving ladder, the multiplier, and the
        // follow-tank party-tank lookup all gate on this flag, so the tank
        // stops navigating and followers peel off — exactly like dc off — but
        // all boss progress is preserved for resume.
        context->GetValue<bool>("dungeon clear paused")->Set(true);

        // Stop the in-flight advance glide so it doesn't coast to its endpoint.
        // Only out of combat: mid-fight the combat engine owns movement and we
        // want the current fight to finish before holding.
        if (bot && !bot->IsInCombat())
            HaltAllMovement(bot);

        DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tDungeon clear paused.");
        botAI->DoSpecificAction("dc status", event, true);
        return true;
    }

    // Resume on the same boss. Refuse if anyone is dead (mirrors dc on) so we
    // don't unpause straight into a wipe.
    if (AnyPartyMemberDead(bot))
    {
        botAI->TellError(FirstDeadName(bot) + " is dead — rez and try again.");
        return false;
    }

    // Rebuild the transient navigation cache so Advance starts fresh from the
    // bot's current position. Boss progress (selected boss, skipped set, sticky
    // boss) is deliberately left intact — that's the whole point of resume vs.
    // a fresh dc on.
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<uint32>("dungeon clear stuck ticks")->Set(0u);
    context->GetValue<uint32>("dungeon clear stride rebuild attempts")->Set(0u);
    context->GetValue<uint32>("dungeon clear done-not-engaged ticks")->Set(0u);
    context->GetValue<uint32>("dungeon clear pursuit fail ticks")->Set(0u);
    context->GetValue<uint32>("dungeon clear loot yield start")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    context->GetValue<Position&>("dungeon clear last position")->Get() = Position();
    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    context->GetValue<bool>("dungeon clear paused")->Set(false);

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    std::string const target = next.has_value() ? next->name : "the next boss";
    DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tResumed. Heading to " + target + ".");
    botAI->DoSpecificAction("dc status", event, true);
    return true;
}

