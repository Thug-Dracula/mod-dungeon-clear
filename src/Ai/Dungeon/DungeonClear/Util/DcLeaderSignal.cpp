/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcLeaderSignal.h"

#include "DungeonClearUtil.h"   // DC_PULL_* log macros
#include "DungeonClearMath.h"
#include "DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "AttackersValue.h"
#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "InstanceScript.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Chat.h"
#include "ServerFacade.h"
#include "Timer.h"
#include "World.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"

namespace
{
    // True only when a COMPLETE navmesh route (PATHFIND_NORMAL) exists from the
    // bot to `p`. The scout-trail picker only returns crumbs the follower can
    // actually reach over a generated path (a trail can span a navmesh seam that
    // is short in plan view yet not walkable straight-line). File-local twin of
    // the same helper in DcPullPlanner.
    bool IsNavReachable(Player* bot, Position const& p)
    {
        if (!bot)
            return false;
        PathGenerator gen(bot);
        gen.CalculatePath(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ());
        return gen.GetPathType() == PATHFIND_NORMAL;
    }

    // Leader-election memo. FindLeaderTank is on the hot path: nearly every DC
    // trigger gate of every bot consults it each AI tick (IsEnabled, the
    // cross-bot camp/party-tank reads, the multiplier), and each call walks the
    // whole group with a GET_PLAYERBOT_AI lookup per member — O(triggers x
    // members) per tick, noticeable in a 40-bot raid. The election result is
    // stable on tick timescales, so cache it per (group, map) for a few hundred
    // ms. Correctness is preserved by validating the cached leader on every
    // read (alive, same map, still a tank bot, still in this group) and falling
    // through to a full re-scan the instant validation fails — so a leader
    // death/leave is picked up immediately; only the *election of a different
    // eligible tank* (lower GUID joining, no current-leader change) can lag by
    // up to the TTL, which no consumer is sensitive to. Negative results
    // ("group has no tank bot") are cached too — groups without a tank query
    // just as often. Mutex-guarded like the other file-scope registries: all
    // callers run on the world/map thread today, but the lock is uncontended
    // and keeps this correct if bot updates ever move off-thread.
    constexpr uint32 LEADER_CACHE_TTL_MS = 250;
    // Lazy janitor bound: past this many entries, stale rows (disbanded groups,
    // emptied maps) are swept on the next store.
    constexpr size_t LEADER_CACHE_SWEEP_SIZE = 256;

    struct LeaderCacheEntry
    {
        ObjectGuid leader;   // Empty = cached "no tank bot in this group"
        uint32 stampMs = 0;
    };

    std::map<std::pair<uint64, uint32>, LeaderCacheEntry> g_leaderCache;
    std::mutex g_leaderCacheMutex;

    // The cached GUID is only trusted while the player it names would still win
    // (or at least remain) the election from `reference`'s point of view.
    Player* ValidateCachedLeader(Player* reference, Group* group, ObjectGuid guid)
    {
        if (guid.IsEmpty())
            return nullptr;
        Player* leader = ObjectAccessor::FindPlayer(guid);
        if (!leader || !leader->IsAlive())
            return nullptr;
        if (leader->GetMapId() != reference->GetMapId())
            return nullptr;
        if (leader->GetGroup() != group)
            return nullptr;
        if (!PlayerbotAI::IsTank(leader) || !GET_PLAYERBOT_AI(leader))
            return nullptr;
        return leader;
    }
}

Player* DcLeaderSignal::FindLeaderTank(Player* reference)
{
    if (!reference)
        return nullptr;

    Group* group = reference->GetGroup();
    if (!group)
    {
        // Solo: a tank bot leads itself; anyone else has no leader. Cheap —
        // no group walk, so no caching either.
        return (PlayerbotAI::IsTank(reference) && GET_PLAYERBOT_AI(reference))
                   ? reference : nullptr;
    }

    // The election is per (group, map): members on another map are excluded
    // from the candidate set, so two parties of one raid split across maps can
    // legitimately resolve different leaders.
    std::pair<uint64, uint32> const key(group->GetGUID().GetRawValue(),
                                        reference->GetMapId());
    uint32 const now = getMSTime();

    {
        std::lock_guard<std::mutex> lock(g_leaderCacheMutex);
        auto it = g_leaderCache.find(key);
        if (it != g_leaderCache.end() &&
            getMSTimeDiff(it->second.stampMs, now) < LEADER_CACHE_TTL_MS)
        {
            // Negative hit: this group/map had no eligible tank bot moments
            // ago. Trust it for the TTL; the next restamp flips it the moment
            // one appears.
            if (it->second.leader.IsEmpty())
                return nullptr;
            // Positive hit: trust it only while it still validates. A failed
            // validation (died / left map / left group / AI gone) falls
            // through to a fresh scan below — never a stale leader.
            if (Player* cached = ValidateCachedLeader(reference, group, it->second.leader))
                return cached;
        }
    }

    // Lowest-GUID alive tank bot on the reference's map. GetFirstMember walks
    // every member of the group — including all raid sub-groups — so each member
    // sees the same candidate set and elects the same leader.
    Player* leader = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive())
            continue;
        if (member->GetMapId() != reference->GetMapId())
            continue;
        if (!PlayerbotAI::IsTank(member))
            continue;
        // Only bot tanks can drive — a real-player tank has no PlayerbotAI to
        // run the clear, so it can never be the leader.
        if (!GET_PLAYERBOT_AI(member))
            continue;
        if (!leader || member->GetGUID() < leader->GetGUID())
            leader = member;
    }

    {
        std::lock_guard<std::mutex> lock(g_leaderCacheMutex);
        if (g_leaderCache.size() > LEADER_CACHE_SWEEP_SIZE)
        {
            for (auto it = g_leaderCache.begin(); it != g_leaderCache.end();)
            {
                if (getMSTimeDiff(it->second.stampMs, now) >= LEADER_CACHE_TTL_MS)
                    it = g_leaderCache.erase(it);
                else
                    ++it;
            }
        }
        g_leaderCache[key] = LeaderCacheEntry{
            leader ? leader->GetGUID() : ObjectGuid::Empty, now};
    }
    return leader;
}
bool DcLeaderSignal::IsDungeonClearLeader(Player* bot)
{
    return bot && FindLeaderTank(bot) == bot;
}
bool DcLeaderSignal::IsInPausedDungeonClearRun(Player* bot)
{
    if (!bot)
        return false;

    // Resolve the run owner from any member (the leader resolves to itself).
    Player* leader = FindLeaderTank(bot);
    if (!leader)
        return false;

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* leaderCtx = leaderAI->GetAiObjectContext();
    return leaderCtx->GetValue<bool>("dungeon clear enabled")->Get() &&
           leaderCtx->GetValue<bool>("dungeon clear paused")->Get();
}
bool DcLeaderSignal::IsPullPhaseHolding(uint32 phase)
{
    return phase == static_cast<uint32>(DcPullPhase::Forming) ||
           phase == static_cast<uint32>(DcPullPhase::Advancing) ||
           phase == static_cast<uint32>(DcPullPhase::Returning);
}
bool DcLeaderSignal::GetLeaderPullInfo(Player* bot, uint32& phaseOut, Position& campOut)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader)
        return false;

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    // Pull behavior only matters while the run is live and pull mode is on. A
    // paused/off leader holds the whole party via the normal gates — EXCEPT
    // while a maneuver phase is actually holding the party at camp: `dc pause`
    // mid-drag deliberately lets the drag finish (abandoning the tank with a
    // live pack on it is worse; see DungeonClearPullManeuverTrigger), so the
    // holding signal must survive the pause or the followers would be released
    // from camp — and ReapStrandedPassives would strip their passive — while
    // the tank is still dragging the pack home. Once the phase resolves to
    // Engage/Idle the paused gate takes over and the run holds as usual.
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        !ctx->GetValue<bool>("dungeon clear pull mode")->Get())
        return false;

    DcPullContext const& pull = ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    if (pull.phase == DcPullPhase::Idle)
        return false;
    if (!IsPullPhaseHolding(static_cast<uint32>(pull.phase)) &&
        ctx->GetValue<bool>("dungeon clear paused")->Get())
        return false;

    phaseOut = static_cast<uint32>(pull.phase);
    campOut = pull.camp;
    return true;
}
bool DcLeaderSignal::GetLeaderCampHold(Player* bot, Position& campOut, bool& passiveOut)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader drives; it never holds at its own camp

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        !ctx->GetValue<bool>("dungeon clear pull mode")->Get())
        return false;

    DcPullContext const& pull = ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    // Honor `paused` only while NO maneuver phase is holding: a pause mid-drag
    // lets the drag finish (see DungeonClearPullManeuverTrigger), and the party
    // must stay pinned at camp until it resolves to Engage — releasing them
    // here would walk them into the inbound pack. Same rule as
    // GetLeaderPullInfo above; `enabled` stays first so `dc off` still releases
    // everyone instantly.
    if (!IsPullPhaseHolding(static_cast<uint32>(pull.phase)) &&
        ctx->GetValue<bool>("dungeon clear paused")->Get())
        return false;
    Position const camp = pull.camp;
    // No camp marked yet (pull mode just toggled on, or a reset cleared it): there
    // is nothing to hold at, so let the caller fall back (briefly) to follow.
    if (camp.GetPositionX() == 0.0f && camp.GetPositionY() == 0.0f &&
        camp.GetPositionZ() == 0.0f)
        return false;

    campOut = camp;
    passiveOut = IsPullPhaseHolding(static_cast<uint32>(pull.phase));
    return true;
}
bool DcLeaderSignal::IsLeaderCampFightActive(Player* bot)
{
    if (!bot || bot->isDead())
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader runs the fight; it never assists itself

    uint32 phase = 0;
    Position camp;
    if (!GetLeaderPullInfo(bot, phase, camp))
        return false;

    // Only the camp fight (Engage) — during the holding phases the party is
    // pinned passive at camp by hold-at-camp/stay-at-camp, and a leader-in-combat
    // while merely scouting (Idle) is handled by the drag-back maneuver, which
    // flips the phase out of Idle before any party member would assist.
    return phase == static_cast<uint32>(DcPullPhase::Engage) && leader->IsInCombat();
}
bool DcLeaderSignal::IsLeaderFightAssistWanted(Player* bot)
{
    // Advanced-pull camp fight: the existing Engage-phase gate.
    if (IsLeaderCampFightActive(bot))
        return true;

    if (!bot || bot->isDead())
        return false;

    // Any OTHER advanced-pull camp-hold state defers to the pull machinery:
    // during the holding phases (Forming/Advancing/Returning) the party is
    // pinned passive at camp, and unplanned aggro while scouting (Idle) is
    // dragged back to camp by the maneuver before the party may engage.
    Position camp;
    bool passive = false;
    if (GetLeaderCampHold(bot, camp, passive))
        return false;

    // General case — no camp in effect (pull mode off, a Leeroy verdict in
    // dynamic mode, boss walk-ins): the leader tank fighting ANYTHING on an
    // active run means the whole party assists. This covers tank aggro around
    // a corner or beyond a follower's natural engage range, where group combat
    // never propagates and the (multiplier-suppressed) stock pickers would
    // leave the party idling behind.
    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        ctx->GetValue<bool>("dungeon clear paused")->Get())
        return false;

    return leader->IsInCombat();
}
bool DcLeaderSignal::IsLeaderDynamicScouting(Player* bot)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader drives; the lag applies to followers only

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        ctx->GetValue<bool>("dungeon clear paused")->Get())
        return false;

    // Dynamic mode only (pull setting == 2). Off/On have no scouting-then-decide
    // window — the party either always follows close (Off) or always holds at camp
    // (On) — so the lag would only ever delay them for no benefit there.
    if (ctx->GetValue<uint32>("dungeon clear pull setting")->Get() != 2u)
        return false;

    // Still scouting: the verdict for the upcoming pack hasn't been committed. Once
    // the tank tags the pack it enters combat, and an Advanced verdict marks a camp
    // (handing the party to hold-at-camp) — either way the phase leaves Idle and the
    // party stops lagging and engages.
    if (leader->IsInCombat())
        return false;
    DcPullContext const& pull =
        ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    if (pull.phase != DcPullPhase::Idle)
        return false;

    // Leeroy roll-in: with a standing Leeroy verdict, release the lag as the tank
    // closes to commit range + PullDynamicRollInLead on the verdicted pack — the
    // tank is committing to the charge, so the party closes the remaining gap
    // DURING the final approach and arrives roughly with first contact, instead
    // of standing flat-footed at the lag ring until combat registers and only
    // then starting a 15-20yd run. Advanced verdicts are untouched: the camp
    // machinery owns the party there (and the Advanced flip after a release is
    // covered too — hold-at-camp outranks follow-tank, so the party walks back).
    if (!pull.decisionTarget.IsEmpty())
    {
        Unit* target = ObjectAccessor::GetUnit(*leader, pull.decisionTarget);
        bool const targetAlive = target && target->IsAlive();
        float const tankToTarget = targetAlive ? leader->GetExactDist2d(target) : 0.0f;
        float const commitRange = targetAlive
            ? DcEngageGeometry::PullCommitRange(leader, target, DC_PULL_START_RANGE)
            : 0.0f;
        float const lead = DcSettings::GetFloat(leader, "PullDynamicRollInLead");
        if (DungeonClearMath::ShouldRollInForLeeroy(pull.decision, targetAlive,
                                                    tankToTarget, commitRange, lead))
        {
            DC_PULL_TRACE("[DC:{}] scout-lag: leeroy roll-in — tank {:.1f}yd from "
                          "pack (commit {:.1f} + lead {:.1f}) -> lag released",
                          bot->GetName(), tankToTarget, commitRange, lead);
            return false;
        }
    }
    return true;
}
bool DcLeaderSignal::GetLeaderScoutTrailPoint(Player* bot, float lag, Position& out)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader drives; only followers trail it

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    // The trail lives in the LEADER's context — only the tank runs Advance and so
    // only the tank records breadcrumbs.
    std::vector<Position> const& crumbs =
        ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get().breadcrumbs;
    if (crumbs.empty())
        return false;

    Position const tankPos(leader->GetPositionX(), leader->GetPositionY(),
                           leader->GetPositionZ());

    // Walk BACK along the trail (newest -> oldest) accumulating real walked distance,
    // exactly like ComputeTrailCamp, and return the first reachable crumb at least
    // `lag` yards behind the tank. A 3D segment > kJumpGuard is a drag/teleport seam
    // — stop, nothing beyond it is contiguously "behind" the tank.
    //
    // Reachability (a full PathGenerator build per probe) is deliberately tested
    // LAZILY: only the crumbs at/past the lag distance are probed on the walk, and
    // the pre-lag crumbs only as a farthest-first fallback when no post-lag crumb
    // was reachable (trail shorter than the lag, or every far crumb across a seam).
    // The previous version probed every crumb as it walked, which cost one navmesh
    // path build PER CRUMB per follower per scout tick; the selected crumb is
    // identical either way, but the typical tick now runs exactly one probe.
    constexpr float kJumpGuard = 12.0f;
    std::vector<std::pair<float, Position>> preLag;  // (along, crumb), nearest-first
    Position prev = tankPos;
    float along = 0.0f;
    for (std::size_t i = crumbs.size(); i-- > 0; )
    {
        Position const& c = crumbs[i];
        float const seg = prev.GetExactDist(&c);
        prev = c;
        if (seg > kJumpGuard)
            break;  // discontinuity behind us — stop here
        along += seg;
        if (along < lag)
        {
            preLag.emplace_back(along, c);
            continue;
        }
        // Only ever trail to a crumb the follower can reach over a complete
        // generated path; a crumb across a navmesh seam would straight-line the
        // move under the map.
        if (IsNavReachable(bot, c))
        {
            out = c;
            return true;
        }
    }

    // Trail shorter than the full lag (or nothing reachable past it): trail the
    // farthest reachable pre-lag crumb (the follower simply stacks a little
    // closer until more trail accrues).
    for (auto it = preLag.rbegin(); it != preLag.rend(); ++it)
    {
        if (IsNavReachable(bot, it->second))
        {
            out = it->second;
            return true;
        }
    }
    return false;
}
void DcLeaderSignal::AbortLeaderPull(Player* bot)
{
    if (!bot)
        return;
    Player* leader = FindLeaderTank(bot);
    if (!leader)
        return;
    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return;
    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    DcPullContext& pull = ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    if (IsPullPhaseHolding(static_cast<uint32>(pull.phase)))
    {
        // Through EnterEngage (not a bare phase write) so the per-pull tag latch
        // is cleared on this Engage entry too — same rule as DcSetPullPhase.
        pull.EnterEngage(getMSTime());
        DC_PULL_INFO("[DC:{}] advanced-pull: leader pull aborted (forced to Engage) "
                     "-> party released", leader->GetName());
    }
}
void DcLeaderSignal::SetLeaderDazeImmunity(Player* leader, bool apply)
{
    if (!leader)
        return;

    // Block all spells carrying the Daze mechanic (spell 1604 is the only one
    // creatures apply). spellId 0 = a blanket mechanic block. Pair add/remove
    // exactly: remove first so a re-apply never stacks duplicate entries.
    leader->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_DAZE, false);
    if (apply)
    {
        leader->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_DAZE, true);
        // Immunity only blocks FUTURE applications; clear any Daze already on
        // the tank from before pull mode came on (or before the drag started).
        leader->RemoveAurasDueToSpell(1604);
    }
}
