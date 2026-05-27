/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "AttackersValue.h"
#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Chat.h"
#include "ServerFacade.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"

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
    std::vector<std::pair<G3D::Vector3, G3D::Vector3>> segments;
    segments.reserve(corridor.size());
    float accumulated = 0.0f;
    for (size_t i = 0; i + 1 < corridor.size(); ++i)
    {
        G3D::Vector3 const& a = corridor[i];
        G3D::Vector3 const& b = corridor[i + 1];
        segments.emplace_back(a, b);
        float const dx = b.x - a.x;
        float const dy = b.y - a.y;
        accumulated += std::sqrt(dx * dx + dy * dy);
        if (accumulated >= maxLookAhead)
            break;
    }
    if (segments.empty())
        return nullptr;

    float const widthSq = corridorWidth * corridorWidth;
    Unit* best = nullptr;
    float bestDistFromBot = std::numeric_limits<float>::max();

    for (ObjectGuid guid : possibleTargets)
    {
        Unit* u = ObjectAccessor::GetUnit(*bot, guid);
        if (!u || !u->IsAlive())
            continue;
        if (!bot->IsHostileTo(u))
            continue;

        float const ux = u->GetPositionX();
        float const uy = u->GetPositionY();

        float minDistSq = std::numeric_limits<float>::max();
        for (auto const& seg : segments)
        {
            float const d2 = DistSqToSegment2D(ux, uy, seg.first.x, seg.first.y, seg.second.x, seg.second.y);
            if (d2 < minDistSq)
                minDistSq = d2;
        }
        if (minDistSq > widthSq)
            continue;

        float const distFromBot = bot->GetDistance2d(u);
        if (distFromBot < bestDistFromBot && IsLevelReachable(bot, u))
        {
            best = u;
            bestDistFromBot = distFromBot;
        }
    }

    // LOS gate. Drops "pack on the other side of a wall" false positives
    // that the geometry-only test would otherwise produce — the corridor
    // may legitimately turn behind a wall, but anything we engage has to
    // be visible from the bot right now.
    if (best && !bot->IsWithinLOSInMap(best))
        return nullptr;

    return best;
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

    // Walk the polyline starting from the bot's current position. Stop
    // accumulating once we've traveled `maxLookAhead` yards along it —
    // anything past that isn't blocking our immediate next pull.
    struct Seg2D { float ax, ay, bx, by; };
    std::vector<Seg2D> segs;
    segs.reserve(segments.size());

    float prevX = bot->GetPositionX();
    float prevY = bot->GetPositionY();
    float accumulated = 0.0f;
    for (PathSegment const& seg : segments)
    {
        float const dx = seg.ex - prevX;
        float const dy = seg.ey - prevY;
        float const len = std::sqrt(dx * dx + dy * dy);
        segs.push_back(Seg2D{prevX, prevY, seg.ex, seg.ey});
        accumulated += len;
        prevX = seg.ex;
        prevY = seg.ey;
        if (accumulated >= maxLookAhead)
            break;
    }
    if (segs.empty())
        return nullptr;

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

        float minDistSq = std::numeric_limits<float>::max();
        for (auto const& seg : segs)
        {
            float const d2 = DistSqToSegment2D(ux, uy, seg.ax, seg.ay, seg.bx, seg.by);
            if (d2 < minDistSq)
                minDistSq = d2;
        }
        if (minDistSq > widthSq)
            continue;

        float const distFromBot = bot->GetDistance2d(u);
        if (distFromBot < bestDistFromBot && IsLevelReachable(bot, u))
        {
            best = u;
            bestDistFromBot = distFromBot;
        }
    }

    if (best && !bot->IsWithinLOSInMap(best))
        return nullptr;

    return best;
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

