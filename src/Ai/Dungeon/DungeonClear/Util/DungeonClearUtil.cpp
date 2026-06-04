/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "AttackersValue.h"
#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "InstanceScript.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Chat.h"
#include "ServerFacade.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"

Unit* DungeonClearUtil::FindBlockingTrash(Player* bot,
                                          DungeonBossInfo const& boss,
                                          float range,
                                          float halfAngle,
                                          GuidVector const& possibleTargets)
{
    if (!bot || possibleTargets.empty())
        return nullptr;

    float const bossDx = boss.x - bot->GetPositionX();
    float const bossDy = boss.y - bot->GetPositionY();
    float const bossAngle = std::atan2(bossDy, bossDx);

    Unit* best = nullptr;
    float bestDist = range;

    for (ObjectGuid guid : possibleTargets)
    {
        Unit* u = ObjectAccessor::GetUnit(*bot, guid);
        if (!u || !u->IsAlive())
            continue;
        if (!bot->IsHostileTo(u))
            continue;
        // Deliberately do NOT skip in-combat units. A hostile in our forward
        // corridor that's already engaged on another party member is *more*
        // reason to engage, not less — otherwise the tank walks past while
        // the healer gets clawed.

        float const dx = u->GetPositionX() - bot->GetPositionX();
        float const dy = u->GetPositionY() - bot->GetPositionY();
        float const dist = std::hypot(dx, dy);
        if (dist > range)
            continue;

        float const ang = std::atan2(dy, dx);
        float delta = std::fabs(ang - bossAngle);
        if (delta > static_cast<float>(M_PI))
            delta = 2.0f * static_cast<float>(M_PI) - delta;
        if (delta > halfAngle)
            continue;

        if (dist < bestDist && IsLevelReachable(bot, u))
        {
            best = u;
            bestDist = dist;
        }
    }
    return best;
}

bool DungeonClearUtil::ComputeCorridor(Player* bot,
                                       float bx, float by, float bz,
                                       Movement::PointsArray& out)
{
    out.clear();
    if (!bot)
        return false;
    PathGenerator gen(bot);
    gen.CalculatePath(bx, by, bz);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;
    out = gen.GetPath();
    return out.size() >= 2;
}

namespace
{
    // A 2D line segment of the lookahead corridor (a→b), projected onto the XY
    // plane. Both corridor-trash scanners build a flat list of these and hand
    // it to PickBlockingTrash.
    struct Seg2D { float ax, ay, bx, by; };

    // Squared 2D distance from point P to segment (A,B).
    float DistSqToSegment2D(float px, float py,
                            float ax, float ay,
                            float bx, float by)
    {
        float const ex = bx - ax;
        float const ey = by - ay;
        float const len2 = ex * ex + ey * ey;
        if (len2 <= 1e-6f)
        {
            float const dx = px - ax;
            float const dy = py - ay;
            return dx * dx + dy * dy;
        }
        float t = ((px - ax) * ex + (py - ay) * ey) / len2;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        float const cx = ax + t * ex;
        float const cy = ay + t * ey;
        float const dx = px - cx;
        float const dy = py - cy;
        return dx * dx + dy * dy;
    }

    // Shared candidate evaluation for the corridor-style trash scans. Returns
    // the closest hostile, alive, level-reachable, in-LOS unit whose 2D
    // position lands within `corridorWidth` of any segment in `segs`. Nullptr
    // if none.
    //
    // LOS (and level-reachability) are checked PER CANDIDATE rather than once
    // on the final pick: a geometrically closer mob behind a wall must not
    // shadow a visible blocker further along the corridor — otherwise the tank
    // walks right past the visible pack and aggros it onto the healers.
    Unit* PickBlockingTrash(Player* bot,
                            std::vector<Seg2D> const& segs,
                            float corridorWidth,
                            GuidVector const& candidates)
    {
        if (!bot || segs.empty() || candidates.empty())
            return nullptr;

        // 2D AABB around the lookahead corridor, padded by the width. Units
        // nowhere near the route get rejected with four float compares before
        // any per-segment distance math runs.
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        for (Seg2D const& s : segs)
        {
            minX = std::min({minX, s.ax, s.bx});
            maxX = std::max({maxX, s.ax, s.bx});
            minY = std::min({minY, s.ay, s.by});
            maxY = std::max({maxY, s.ay, s.by});
        }
        minX -= corridorWidth; maxX += corridorWidth;
        minY -= corridorWidth; maxY += corridorWidth;

        float const widthSq = corridorWidth * corridorWidth;
        Unit* best = nullptr;
        float bestDistFromBot = std::numeric_limits<float>::max();

        for (ObjectGuid guid : candidates)
        {
            Unit* u = ObjectAccessor::GetUnit(*bot, guid);
            if (!u || !u->IsAlive())
                continue;
            if (!bot->IsHostileTo(u))
                continue;

            float const ux = u->GetPositionX();
            float const uy = u->GetPositionY();

            // Trivial AABB reject before any sqrt / segment math.
            if (ux < minX || ux > maxX || uy < minY || uy > maxY)
                continue;

            // We only want the closest blocker; once we have one, a farther
            // candidate can't win, so skip the corridor/LOS/path work for it.
            float const distFromBot = bot->GetDistance2d(u);
            if (distFromBot >= bestDistFromBot)
                continue;

            bool inCorridor = false;
            for (Seg2D const& s : segs)
            {
                if (DistSqToSegment2D(ux, uy, s.ax, s.ay, s.bx, s.by) <= widthSq)
                {
                    inCorridor = true;
                    break;  // threshold test — first segment in range is enough
                }
            }
            if (!inCorridor)
                continue;

            // Expensive gates last, and per-candidate so a closer out-of-LOS
            // mob can't shadow a visible one further along the corridor.
            if (!DungeonClearUtil::IsLevelReachable(bot, u))
                continue;
            if (!bot->IsWithinLOSInMap(u))
                continue;

            best = u;
            bestDistFromBot = distFromBot;
        }
        return best;
    }
}

Unit* DungeonClearUtil::FindBlockingTrashCorridor(Player* bot,
                                                  Movement::PointsArray const& corridor,
                                                  float maxLookAhead,
                                                  float corridorWidth,
                                                  GuidVector const& possibleTargets)
{
    if (!bot || corridor.size() < 2 || possibleTargets.empty())
        return nullptr;

    // Walk the polyline only as far as `maxLookAhead` yards from the bot's
    // current position. Beyond that the trigger doesn't care — we only
    // engage things that actually block the next leg of the route.
    std::vector<Seg2D> segments;
    segments.reserve(corridor.size());
    float accumulated = 0.0f;
    for (size_t i = 0; i + 1 < corridor.size(); ++i)
    {
        G3D::Vector3 const& a = corridor[i];
        G3D::Vector3 const& b = corridor[i + 1];
        segments.push_back(Seg2D{a.x, a.y, b.x, b.y});
        float const dx = b.x - a.x;
        float const dy = b.y - a.y;
        accumulated += std::sqrt(dx * dx + dy * dy);
        if (accumulated >= maxLookAhead)
            break;
    }

    // Per-candidate LOS gate inside PickBlockingTrash drops "pack on the other
    // side of a wall" false positives without letting a closer out-of-LOS mob
    // shadow a visible blocker further along the corridor.
    return PickBlockingTrash(bot, segments, corridorWidth, possibleTargets);
}

Creature* DungeonClearUtil::FindLiveCreatureOnMap(Player* bot, uint32 entry)
{
    if (!bot)
        return nullptr;
    Map* map = bot->GetMap();
    if (!map)
        return nullptr;

    for (auto const& kv : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = kv.second;
        if (c && c->GetEntry() == entry && c->IsAlive())
            return c;
    }
    return nullptr;
}

Creature* DungeonClearUtil::GetLiveBoss(Player* bot, AiObjectContext* ctx, uint32 entry)
{
    if (!bot || !ctx || !entry)
        return nullptr;

    DungeonClearLiveBoss const cached =
        ctx->GetValue<DungeonClearLiveBoss>("dungeon clear live boss")->Get();
    if (cached.entry == entry && !cached.guid.IsEmpty())
    {
        // Re-resolve the cached GUID every call so the position stays live and
        // we never touch a pointer that may have been freed since the scan.
        Creature* c = ObjectAccessor::GetCreature(*bot, cached.guid);
        if (c && c->IsAlive() && c->GetEntry() == entry)
            return c;
        // Cached GUID went stale within the interval (died / despawned). Fall
        // through to a direct scan rather than miss a still-present instance.
    }

    // Cache miss: computed for a different boss (just after a boss change), or
    // a stale GUID — pay one direct scan. Steady state hits the branch above.
    return FindLiveCreatureOnMap(bot, entry);
}

bool DungeonClearUtil::IsCreaturePresentOnMap(Player* bot, uint32 entry)
{
    if (!bot)
        return false;
    Map* map = bot->GetMap();
    if (!map)
        return false;

    for (auto const& kv : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = kv.second;
        if (c && c->GetEntry() == entry)
            return true;
    }
    return false;
}

bool DungeonClearUtil::IsReachable(Player* bot, float x, float y, float z)
{
    // Delegate to the chunked pathfinder. Strict PATHFIND_NORMAL was too
    // restrictive — most dungeon boss-rooms exceed PathGenerator's ~296yd
    // single-call cap, so the strict check returned false for every
    // medium-distance boss and the tank stalled with "no navigable route"
    // before getting a chance to walk the first chunk.
    //
    // The chunked check accepts a path with any forward progress and lets
    // Advance walk the remaining chunks on subsequent ticks. The position-
    // based stuck detector still catches truly unreachable destinations.
    return ChunkedPathfinder::IsReachable(bot, x, y, z);
}

bool DungeonClearUtil::IsLevelReachable(Player* bot, Unit* u)
{
    if (!bot || !u)
        return false;

    // Below this vertical offset the candidate is on the bot's own level:
    // slopes, stairs and ramps stay under it within a corridor lookahead, so
    // we trust the caller's 2D corridor/cone/LOS test and skip the probe.
    // WotLK inter-floor gaps are larger than this, so a genuine other-level
    // mob always falls through to the pathfinder check below.
    constexpr float DC_Z_LEVEL_TOLERANCE = 5.0f;

    float const dz = std::fabs(u->GetPositionZ() - bot->GetPositionZ());
    if (dz <= DC_Z_LEVEL_TOLERANCE)
        return true;

    // Different vertical level: confirm an actual ground path. A single direct
    // probe suffices — trash is always inside the corridor/cone lookahead,
    // comfortably under PathGenerator's single-call range cap.
    PathGenerator gen(bot);
    gen.CalculatePath(u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(), /*forceDest*/ false);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;

    // PathGenerator clamps an off-mesh destination to the nearest walkable
    // poly, which on layered geometry can be the bot's *own* floor directly
    // above/below the mob — yielding a bogus NORMAL path that never descends.
    // Require the route to actually end on the candidate's level.
    Movement::PointsArray const& path = gen.GetPath();
    if (path.empty())
        return false;
    G3D::Vector3 const& end = path.back();
    return std::fabs(end.z - u->GetPositionZ()) <= DC_Z_LEVEL_TOLERANCE;
}

Unit* DungeonClearUtil::FindBlockingTrashOnPath(Player* bot,
                                                std::vector<PathSegment> const& segments,
                                                float maxLookAhead,
                                                float corridorWidth,
                                                GuidVector const& candidates)
{
    if (!bot || segments.empty() || candidates.empty())
        return nullptr;

    // Walk the smoothed route starting from the bot's current position. Stop
    // accumulating once we've traveled `maxLookAhead` yards along it — anything
    // past that isn't blocking our immediate next pull.
    //
    // CRITICAL: chain each segment's `polyline` points, NOT the segment
    // endpoints (`seg.ex/ey`). The primary producer (LongRangePathfinder) emits
    // the ENTIRE winding route as a single PathSegment whose ex/ey is only the
    // final endpoint (the boss). Chaining endpoints would collapse the corridor
    // to one straight bee-line from the bot to the boss — a cylinder that cuts
    // through walls and rooms, runs the whole route in one segment, and ignores
    // maxLookAhead. The polyline is the real smoothed corridor geometry. (Same
    // fix already applied in DungeonClearBlockingDoorValue::Calculate.)
    std::vector<Seg2D> segs;
    segs.reserve(segments.size());

    float prevX = bot->GetPositionX();
    float prevY = bot->GetPositionY();
    float accumulated = 0.0f;
    bool limitReached = false;
    for (PathSegment const& seg : segments)
    {
        // Anchored segments collapse to a single polyline point; non-anchored
        // segments carry the full smoothed corridor. Fall back to the endpoint
        // only if a segment somehow has no polyline at all.
        if (seg.polyline.empty())
        {
            float const dx = seg.ex - prevX;
            float const dy = seg.ey - prevY;
            segs.push_back(Seg2D{prevX, prevY, seg.ex, seg.ey});
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = seg.ex;
            prevY = seg.ey;
            if (accumulated >= maxLookAhead)
                break;
            continue;
        }

        for (G3D::Vector3 const& pt : seg.polyline)
        {
            float const dx = pt.x - prevX;
            float const dy = pt.y - prevY;
            segs.push_back(Seg2D{prevX, prevY, pt.x, pt.y});
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = pt.x;
            prevY = pt.y;
            if (accumulated >= maxLookAhead)
            {
                limitReached = true;
                break;
            }
        }
        if (limitReached)
            break;
    }

    return PickBlockingTrash(bot, segs, corridorWidth, candidates);
}

Unit* DungeonClearUtil::FindNearestReachableHostile(Player* bot)
{
    if (!bot)
        return nullptr;
    Map* map = bot->GetMap();
    if (!map)
        return nullptr;

    // Match the cap PullRequestAction enforces — anything past this gets
    // silently rejected by the pull pipeline, so don't even consider it.
    float const maxPullDistance = sPlayerbotAIConfig.reactDistance * 3.0f;

    // Collect candidate hostiles from every loaded creature on the map, sort
    // by distance, then take the first one we can actually path to AND that
    // the pull pipeline will accept. Cheap pre-sort means we usually only
    // run 1–2 PathGenerator calls.
    std::vector<std::pair<float, Creature*>> candidates;
    for (auto const& kv : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = kv.second;
        if (!c || !c->IsAlive())
            continue;
        if (!bot->IsHostileTo(c))
            continue;
        if (c->IsInCombat())
            continue;
        float const dist = bot->GetDistance(c);
        if (dist > maxPullDistance)
            continue;
        // Apply the same target validity gates the pull pipeline uses
        // (visibility, faction edge cases, non-attackable flags, etc.).
        // Without this we'd hand RequestPull a target it silently rejects
        // downstream and the tank would just stand there.
        if (!AttackersValue::IsPossibleTarget(c, bot))
            continue;
        candidates.emplace_back(dist, c);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    for (auto const& pair : candidates)
    {
        Creature* c = pair.second;
        if (IsReachable(bot, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ()))
            return c;
    }
    return nullptr;
}

InstanceScript* DungeonClearUtil::GetInstanceScript(Player* bot)
{
    if (!bot)
        return nullptr;
    Map* map = bot->GetMap();
    if (!map)
        return nullptr;
    InstanceMap* im = map->ToInstanceMap();
    return im ? im->GetInstanceScript() : nullptr;
}

bool DungeonClearUtil::IsPartyReady(Player* bot, float minHpPct, float minMpPct, float maxSpread)
{
    if (!bot)
        return false;
    Group* group = bot->GetGroup();
    if (!group)
        return true;  // Solo tank — always "ready."

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        if (member->GetMapId() != bot->GetMapId())
            continue;
        if (member->isDead())
            continue;  // Dead members handled by the party-died trigger.

        if (member != bot && bot->GetDistance(member) > maxSpread)
            return false;
        if (member->GetHealthPct() < minHpPct)
            return false;
        if (member->getPowerType() == POWER_MANA)
        {
            uint32 const maxMp = member->GetMaxPower(POWER_MANA);
            if (maxMp > 0)
            {
                float const mpPct = 100.0f * float(member->GetPower(POWER_MANA)) / float(maxMp);
                if (mpPct < minMpPct)
                    return false;
            }
        }
    }
    return true;
}

bool DungeonClearUtil::IsAnyPartyMemberLooting(Player* bot)
{
    if (!bot)
        return false;
    Group* group = bot->GetGroup();
    if (!group)
        return false;  // Solo tank — no followers to wait on.

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;
        if (!member->IsAlive())
            continue;
        if (member->GetMapId() != bot->GetMapId())
            continue;

        // Only bot members loot under our coordination; a real player has no
        // PlayerbotAI, so we can't read their loot intent and don't wait on it.
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;

        AiObjectContext* memberCtx = memberAI->GetAiObjectContext();
        if (memberCtx->GetValue<bool>("can loot")->Get() ||
            memberCtx->GetValue<bool>("has available loot")->Get())
            return true;
    }
    return false;
}

std::string DungeonClearUtil::DescribePartyNotReady(Player* bot,
                                                    float minHpPct, float minMpPct,
                                                    float maxSpread)
{
    if (!bot)
        return "";
    Group* group = bot->GetGroup();
    if (!group)
        return "";  // Solo tank — nobody to wait on.

    // Keep the addon line short: name a few members, then collapse the rest.
    constexpr size_t MAX_NAMED = 3;
    std::vector<std::string> parts;
    size_t extra = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        if (member->GetMapId() != bot->GetMapId())
            continue;
        if (member->isDead())
            continue;  // Dead members handled by the party-died trigger.

        // Mirror IsPartyReady's checks, but record the limiting reason. Order
        // matters only for which single reason we surface first; distance reads
        // most intuitively, then health, then mana.
        std::string reason;
        if (member != bot && bot->GetDistance(member) > maxSpread)
            reason = "out of range";
        else if (member->GetHealthPct() < minHpPct)
            reason = "low HP";
        else if (member->getPowerType() == POWER_MANA)
        {
            uint32 const maxMp = member->GetMaxPower(POWER_MANA);
            if (maxMp > 0)
            {
                float const mpPct = 100.0f * float(member->GetPower(POWER_MANA)) / float(maxMp);
                if (mpPct < minMpPct)
                    reason = "low mana";
            }
        }

        if (reason.empty())
            continue;  // This member is ready — not blocking.

        if (parts.size() < MAX_NAMED)
            parts.push_back(member->GetName() + " (" + reason + ")");
        else
            ++extra;
    }

    if (parts.empty())
        return "";

    std::string out = "Waiting on ";
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            out += ", ";
        out += parts[i];
    }
    if (extra)
        out += " +" + std::to_string(extra) + " more";
    return out;
}

std::string DungeonClearUtil::DescribePartyLooting(Player* bot)
{
    if (!bot)
        return "";
    Group* group = bot->GetGroup();
    if (!group)
        return "";

    constexpr size_t MAX_NAMED = 1;
    std::vector<std::string> names;
    size_t extra = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;
        if (!member->IsAlive())
            continue;
        if (member->GetMapId() != bot->GetMapId())
            continue;

        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;  // Real player — we don't drive or wait on their loot.

        AiObjectContext* memberCtx = memberAI->GetAiObjectContext();
        if (!memberCtx->GetValue<bool>("can loot")->Get() &&
            !memberCtx->GetValue<bool>("has available loot")->Get())
            continue;

        if (names.size() < MAX_NAMED)
            names.push_back(member->GetName());
        else
            ++extra;
    }

    if (names.empty())
        return "";

    std::string out;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i)
            out += ", ";
        out += names[i];
    }
    if (extra)
        out += " +" + std::to_string(extra) + " more";
    out += " looting";
    return out;
}

void DungeonClearUtil::StripSkippedLoot(PlayerbotAI* botAI)
{
    if (!botAI)
        return;

    AiObjectContext* ctx = botAI->GetAiObjectContext();
    std::map<ObjectGuid, uint32>& skip =
        ctx->GetValue<std::map<ObjectGuid, uint32>&>("dungeon clear loot skip")->Get();
    if (skip.empty())
        return;  // happy path: nothing was ever given up on.

    uint32 const now = getMSTime();
    LootObjectStack* stack = ctx->GetValue<LootObjectStack*>("available loot")->Get();

    for (auto it = skip.begin(); it != skip.end();)
    {
        if (now >= it->second)
        {
            // Expired — drop it so the loot becomes eligible again (a pending
            // group roll may have resolved while we were away).
            it = skip.erase(it);
            continue;
        }
        if (stack)
            stack->Remove(it->first);  // keep it out of "has available loot"
        ++it;
    }

    // can-loot reads "loot target", not the stack, so a bot parked within 3yd
    // of a skipped corpse would keep can-loot true (and keep yielding) unless we
    // also drop the committed target here.
    LootObject const target = ctx->GetValue<LootObject>("loot target")->Get();
    if (!target.guid.IsEmpty() && skip.find(target.guid) != skip.end())
        ctx->GetValue<LootObject>("loot target")->Set(LootObject());
}

void DungeonClearUtil::GiveUpCurrentLoot(PlayerbotAI* botAI, uint32 ttlMs)
{
    if (!botAI)
        return;

    AiObjectContext* ctx = botAI->GetAiObjectContext();

    // Prefer the target stock already committed to; otherwise the nearest loot
    // we'd pick next. Either is what kept the yield armed.
    ObjectGuid guid = ctx->GetValue<LootObject>("loot target")->Get().guid;
    if (guid.IsEmpty())
        if (LootObjectStack* stack = ctx->GetValue<LootObjectStack*>("available loot")->Get())
            guid = stack->GetLoot(sPlayerbotAIConfig.lootDistance).guid;
    if (guid.IsEmpty())
        return;  // nothing of our own to give up on (tank waiting on a follower)

    std::map<ObjectGuid, uint32>& skip =
        ctx->GetValue<std::map<ObjectGuid, uint32>&>("dungeon clear loot skip")->Get();
    skip[guid] = getMSTime() + ttlMs;
}

bool DungeonClearUtil::MaybeGiveUpCampedLoot(PlayerbotAI* botAI, uint32 campTimeoutMs, uint32 giveUpTtlMs)
{
    if (!botAI)
        return false;

    AiObjectContext* ctx = botAI->GetAiObjectContext();
    ObjectGuid& campGuid = ctx->GetValue<ObjectGuid>("dungeon clear loot camp guid")->RefGet();

    // Only meaningful once the bot is standing in interaction range of a corpse
    // (can-loot true). While merely walking toward one (has-available-loot), the
    // broader loot-yield timeout — which budgets for the walk — applies instead.
    if (!ctx->GetValue<bool>("can loot")->Get())
    {
        campGuid = ObjectGuid::Empty;  // not camping -> reset the clock
        return false;
    }

    LootObject const target = ctx->GetValue<LootObject>("loot target")->Get();
    if (target.guid.IsEmpty())
    {
        campGuid = ObjectGuid::Empty;
        return false;
    }

    // Gathering nodes (skinning / mining / herbalism) carry a non-zero skillId
    // and a legitimate multi-second cast — don't mistake the channel for a stuck
    // corpse. Only plain creature / chest loot is subject to the camp cutoff.
    if (target.skillId != 0)
        return false;

    uint32& campStart = ctx->GetValue<uint32>("dungeon clear loot camp start")->RefGet();
    uint32 const now = getMSTime();

    if (campGuid != target.guid)
    {
        // Just arrived at a (new) corpse -> start its camp clock.
        campGuid = target.guid;
        campStart = now;
        return false;
    }

    if (now - campStart < campTimeoutMs)
        return false;  // still within the brief grace a normal pickup needs

    // Standing on the same corpse, in range, well past a normal loot's window:
    // its loot is un-finishable for this bot. Blacklist it now and strip it so
    // the loot flags drop this tick, instead of burning the full loot-yield
    // timeout (and, for the tank, holding the whole party) on it.
    GiveUpCurrentLoot(botAI, giveUpTtlMs);
    StripSkippedLoot(botAI);
    campGuid = ObjectGuid::Empty;
    return true;
}

bool DungeonClearUtil::CorpseHasTakeableLoot(Player* bot, Creature* creature, uint32 minQuality)
{
    if (!bot || !creature)
        return true;  // can't classify -> never skip on our account

    Loot const& loot = creature->loot;

    // Bags full -> only money is takeable (it needs no slot). Managed bots
    // auto-sell so this is rare, but it's a genuine un-finishable case the camp
    // timeout used to absorb. "bag space" is percent USED; 100 == no free slot.
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    bool const bagsFull =
        botAI && botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() >= 100;

    for (LootItem const& item : loot.items)
    {
        if (item.is_looted)
            continue;

        // Won by / reserved for someone else. (A roll we WIN delivers the item
        // automatically; we never come back to the corpse for it, so skipping
        // here loses nothing.)
        if (item.rollWinnerGUID && item.rollWinnerGUID != bot->GetGUID())
            continue;

        // Above-threshold group-loot item under an unresolved roll: not
        // free-lootable by us now (and won items arrive without re-looting).
        if (item.is_blocked && item.rollWinnerGUID != bot->GetGUID())
            continue;

        // Round-robin / explicitly-allowed looter set that excludes us.
        if (!item.freeforall && !item.GetAllowedLooters().empty() &&
            !item.GetAllowedLooters().count(bot->GetGUID()))
            continue;

        // Faction / condition / quest eligibility (mirrors the server's own
        // visibility check).
        if (!item.AllowedForPlayer(bot, creature->GetGUID()))
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
        if (!proto)
            continue;

        // Quest / quest-starter items always qualify, regardless of the floor.
        bool const questItem = item.needs_quest || proto->StartQuest != 0;
        if (!questItem && proto->Quality < minQuality)
            continue;

        if (bagsFull)
            continue;  // would take it, but nowhere to put it

        return true;  // at least one item worth a stop
    }

    // No qualifying item. Gold earns a stop ONLY with no quality floor set
    // (minQuality 0 == stock "loot everything"); once a floor is set, gold-only
    // corpses are skipped so the floor genuinely cuts the number of stops.
    return loot.gold > 0 && minQuality == 0;
}

bool DungeonClearUtil::MaybeSkipUnworthyLoot(PlayerbotAI* botAI, uint32 giveUpTtlMs)
{
    if (!botAI)
        return false;

    Player* bot = botAI->GetBot();
    AiObjectContext* ctx = botAI->GetAiObjectContext();

    // The corpse we're about to commit to: stock's chosen target if set, else
    // the nearest in the stack (what `loot` would pick next). This is the same
    // selection GiveUpCurrentLoot blacklists, so the two stay aligned.
    LootObject target = ctx->GetValue<LootObject>("loot target")->Get();
    if (target.guid.IsEmpty())
        if (LootObjectStack* stack = ctx->GetValue<LootObjectStack*>("available loot")->Get())
            target = stack->GetLoot(sPlayerbotAIConfig.lootDistance);
    if (target.guid.IsEmpty())
        return false;  // nothing pending to judge

    // Gathering nodes carry a skillId and a legitimate cast; chests / GOs / item
    // loot we don't introspect this way. Only plain creature corpses are judged.
    if (target.skillId != 0)
        return false;
    Creature* creature = botAI->GetCreature(target.guid);
    if (!creature)
        return false;

    uint32 const minQuality = sConfigMgr->GetOption<uint32>("DungeonClear.LootMinQuality", 0);

    if (CorpseHasTakeableLoot(bot, creature, minQuality))
        return false;  // worth a stop -> let the loot pipeline run

    // Nothing here for us -> blacklist + strip now so the loot flags drop this
    // tick and the bot skips the detour entirely (the proactive analogue of the
    // camp/yield timeouts firing after a wasted walk).
    GiveUpCurrentLoot(botAI, giveUpTtlMs);
    StripSkippedLoot(botAI);
    return true;
}

void DungeonClearUtil::SendAddonMessage(PlayerbotAI* botAI, std::string const& msg)
{
    Player* bot = botAI->GetBot();
    if (!bot || !bot->GetGroup())
        return;

    std::string const payload = "DC\t" + msg;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, payload.c_str(),
                                 LANG_ADDON, CHAT_TAG_NONE,
                                 bot->GetGUID(), bot->GetName());

    for (Player* receiver : botAI->GetRealPlayersInGroup())
        ServerFacade::instance().SendPacket(receiver, &data);
}

