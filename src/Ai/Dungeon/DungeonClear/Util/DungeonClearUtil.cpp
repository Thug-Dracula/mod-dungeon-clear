/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearUtil.h"
#include "DungeonClearMath.h"

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
#include <unordered_set>
#include <utility>
#include <vector>

#include "AttackersValue.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "InstanceScript.h"
#include "LootObjectStack.h"
#include "Map.h"
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
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
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

Unit* DungeonClearUtil::FindPullTarget(PlayerbotAI* botAI, DungeonBossInfo const& next)
{
    if (!botAI)
        return nullptr;
    Player* bot = botAI->GetBot();
    if (!bot)
        return nullptr;
    AiObjectContext* context = botAI->GetAiObjectContext();

    // Same look-ahead / band the blocking-trash trigger uses; keep them aligned
    // so the pull aims at the very pack the normal flow would otherwise pull.
    constexpr float kLookAhead = 35.0f;
    constexpr float kWidth = 18.0f;
    constexpr float kConeRange = 35.0f;
    float const kConeHalfAngle = static_cast<float>(M_PI) / 3.0f;  // 60°

    GuidVector const& farTargets = context->GetValue<GuidVector>("dungeon clear far targets")->Get();
    GuidVector const& possibleTargets = context->GetValue<GuidVector>("possible targets")->Get();
    GuidVector const& candidates = farTargets.empty() ? possibleTargets : farTargets;

    Unit* trash = nullptr;
    ChunkedPathfinder::Result const& path =
        context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Get();
    if (path.reachable && !path.segments.empty())
    {
        trash = FindBlockingTrashOnPath(bot, path.segments, kLookAhead, kWidth, candidates);
    }
    else
    {
        Movement::PointsArray corridor;
        if (ComputeCorridor(bot, next.x, next.y, next.z, corridor))
            trash = FindBlockingTrashCorridor(bot, corridor, kLookAhead, kWidth, candidates);
        else
            trash = FindBlockingTrash(bot, next, kConeRange, kConeHalfAngle, candidates);
    }

    if (!trash)
        return nullptr;

    // Never pull a pack on the far side of a closed door (mirrors the trigger's
    // veto): some doors are navmesh-passable, so the tank would otherwise run
    // through and drag the pack back through the doorway.
    if (ClosedDoorBetween(bot, trash->GetPositionX(), trash->GetPositionY(), trash->GetPositionZ()))
    {
        DC_PULL_TRACE("[DC:{}] pull target {} vetoed — closed door between us and it",
                      bot->GetName(), trash->GetGUID().ToString());
        return nullptr;
    }

    return trash;
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

    // 2D half-width band around the walked route within which a closed door
    // counts as sitting "on the corridor". Matches DOOR_CORRIDOR_WIDTH in
    // DungeonClearBlockingDoorValue — a door's GO origin (its hinge/jamb) can be
    // several yards off the line the bot actually walks through, so a tighter
    // band misses wide gates.
    constexpr float DC_DOOR_BAND = 8.0f;

    // Vertical tolerance for matching a door to a leg of the route (or to a
    // target). A door's GO origin Z sits at its OWN floor; the route point we
    // test it against is on the walking surface. Beyond this gap the door is on
    // a different level — a stacked ship deck, a ramp above/below — and must not
    // count as blocking. The door tests are otherwise 2D, so without this a door
    // directly over or under a point that is merely near in plan-view falsely
    // blocks the corridor (parks the tank short, vetoes near-side packs). Wide
    // enough to absorb a doorway threshold/lip and minor navmesh-vs-GO Z drift,
    // tight enough to keep adjacent floors apart.
    constexpr float DC_DOOR_Z_BAND = 6.0f;

    // Below this vertical offset a candidate is treated as on the bot's own
    // level: slopes, stairs and ramps stay under it within a corridor
    // lookahead. WotLK inter-floor gaps are larger, so a genuine other-level
    // mob always exceeds it. Shared by IsLevelReachable (trash) and
    // IsAtBossEngage (boss-arrival floor guard).
    constexpr float DC_Z_LEVEL_TOLERANCE = 5.0f;

    // A closed door reduced to its GO-origin point, carried with Z so the
    // corridor tests can reject doors on another floor.
    struct DoorPt { float x, y, z; };

    // Closed `GAMEOBJECT_TYPE_DOOR`s on the map, culled to within `radius` of
    // (cx,cy). Used to truncate the corridor trash scan at the first door so the
    // tank never picks a pack on the FAR side of a shut door — some doors aren't
    // modeled as solid in the navmesh, so the corridor runs straight through
    // them. Mirrors the closed-state logic in DungeonClearBlockingDoorValue.
    std::vector<DoorPt> CollectClosedDoors(Map* map, float cx, float cy,
                                           float radius)
    {
        std::vector<DoorPt> doors;
        if (!map)
            return doors;
        float const r2 = radius * radius;
        for (auto const& kv : map->GetGameObjectBySpawnIdStore())
        {
            GameObject* go = kv.second;
            if (!DungeonClearUtil::IsDoorClosed(go))
                continue;
            float const gx = go->GetPositionX();
            float const gy = go->GetPositionY();
            if ((gx - cx) * (gx - cx) + (gy - cy) * (gy - cy) > r2)
                continue;
            doors.push_back(DoorPt{gx, gy, go->GetPositionZ()});
        }
        return doors;
    }

    // True if any door lies within DC_DOOR_BAND (2D) of segment a→b AND on the
    // segment's floor (door Z within DC_DOOR_Z_BAND of the segment's Z span).
    // The Z gate stops a door one deck up/down — near in plan-view but not on
    // this leg of the route — from truncating the scan. `az`/`bz` are the Z of
    // the segment's two endpoints.
    bool SegmentHitsClosedDoor(Seg2D const& s, float az, float bz,
                               std::vector<DoorPt> const& doors)
    {
        float const bandSq = DC_DOOR_BAND * DC_DOOR_BAND;
        float const loZ = std::min(az, bz) - DC_DOOR_Z_BAND;
        float const hiZ = std::max(az, bz) + DC_DOOR_Z_BAND;
        for (auto const& d : doors)
        {
            if (d.z < loZ || d.z > hiZ)
                continue;
            if (DungeonClearMath::DistSqToSegment2D(d.x, d.y,
                                                    s.ax, s.ay, s.bx, s.by) <= bandSq)
                return true;
        }
        return false;
    }

    // Shared candidate evaluation for the corridor-style trash scans. Returns
    // the closest hostile, alive, level-reachable, in-LOS unit whose 2D
    // position lands within its OWN effective aggro band of any segment in
    // `segs`. Nullptr if none.
    //
    // `corridorWidth` is the band used for every candidate when
    // DungeonClear.DynamicAggroRange is off (legacy fixed-width behaviour).
    // With it on, each candidate's band is its real aggro range (clamped to
    // [DungeonClear.TrashWidthFloor, DungeonClear.TrashWidthCap] via
    // AggroRangeOf) and the AABB cull is padded by that cap: a giant elite with
    // a large aggro radius now counts as blocking even when it sits several
    // yards off the route line, while a low-aggro ambient mob just off the line
    // no longer does — the two failure modes the old single fixed band had.
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

        bool const dynamic =
            DcSettings::GetBool(bot, "DynamicAggroRange");
        float const widthFloor =
            DcSettings::GetFloat(bot, "TrashWidthFloor");
        float const widthCap =
            DcSettings::GetFloat(bot, "TrashWidthCap");

        // Broad-phase band: the widest per-candidate band we might use. With
        // dynamic aggro on it must reach the cap so a giant elite's large band
        // isn't culled by the AABB before its per-candidate test runs; off, it
        // stays at the caller's legacy fixed width.
        float const broadWidth = dynamic ? std::max(corridorWidth, widthCap) : corridorWidth;

        // 2D AABB around the lookahead corridor, padded by the broad-phase
        // width. Units nowhere near the route get rejected with four float
        // compares before any per-segment distance math runs.
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
        minX -= broadWidth; maxX += broadWidth;
        minY -= broadWidth; maxY += broadWidth;

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

            // Per-candidate band: its real aggro range, clamped to
            // [widthFloor, widthCap] so a tiny-aggro mob right on the line
            // still counts and a giant elite is caught out to the cap. Falls
            // back to the caller's fixed corridorWidth when dynamic-aggro is
            // off (AggroRangeOf returns the fallback then).
            float const band =
                DungeonClearUtil::AggroRangeOf(bot, u, corridorWidth, widthFloor, widthCap);
            float const widthSq = band * band;

            bool inCorridor = false;
            for (Seg2D const& s : segs)
            {
                if (DungeonClearMath::DistSqToSegment2D(ux, uy, s.ax, s.ay, s.bx, s.by) <= widthSq)
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
    // Truncate at the first closed door (see FindBlockingTrashOnPath) so a pack
    // on the far side of a shut, navmesh-passable door is never picked.
    auto const doors =
        CollectClosedDoors(bot->GetMap(), bot->GetPositionX(), bot->GetPositionY(),
                           maxLookAhead + DC_DOOR_BAND);

    std::vector<Seg2D> segments;
    segments.reserve(corridor.size());
    float accumulated = 0.0f;
    for (size_t i = 0; i + 1 < corridor.size(); ++i)
    {
        G3D::Vector3 const& a = corridor[i];
        G3D::Vector3 const& b = corridor[i + 1];
        Seg2D const s{a.x, a.y, b.x, b.y};
        if (!doors.empty() && SegmentHitsClosedDoor(s, a.z, b.z, doors))
            break;
        segments.push_back(s);
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

float DungeonClearUtil::AggroRangeOf(Player* bot, Unit* u, float fallback,
                                    float floorYd, float capYd)
{
    if (!bot || !u)
        return fallback;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return fallback;

    Creature* c = u->ToCreature();
    if (!c)
        return fallback;            // players/totems: keep the caller's band

    // Core formula: detection range adjusted by level diff, already clamped
    // 5-45yd. Add the creature's reach so the band measures "how close the
    // route must pass for it to pull", not just its notice distance.
    float range = c->GetAggroRange(bot) + c->GetCombatReach();
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}

float DungeonClearUtil::BossEngageRange(Player* bot, AiObjectContext* ctx,
                                        DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return staticRange;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return staticRange;

    Creature* live = GetLiveBoss(bot, ctx, boss.entry);
    if (!live)
        return staticRange;         // not loaded yet — use the static fallback

    float const margin =
        DcSettings::GetFloat(bot, "AggroRangeMargin");
    float const floorYd =
        DcSettings::GetFloat(bot, "BossEngageRangeFloor");
    float const capYd =
        DcSettings::GetFloat(bot, "BossEngageRangeCap");

    // Hand off as the tank enters the boss's real aggro bubble: its notice
    // distance + both reaches + a small margin so the engage trigger fires
    // just before the boss would pull on its own.
    float range = live->GetAggroRange(bot) + live->GetCombatReach()
                + bot->GetCombatReach() + margin;
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}

bool DungeonClearUtil::IsAtBossEngage(Player* bot, AiObjectContext* ctx,
                                      DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return false;

    Creature* live = GetLiveBoss(bot, ctx, boss.entry);
    float const bx = live ? live->GetPositionX() : boss.x;
    float const by = live ? live->GetPositionY() : boss.y;
    float const bz = live ? live->GetPositionZ() : boss.z;

    float const engageRange = BossEngageRange(bot, ctx, boss, staticRange);

    // Straight-line aggro-bubble gate (same value the trigger ladder reads).
    if (bot->GetDistance(bx, by, bz) >= engageRange)
        return false;

    // Same-floor fast path: within tolerance the straight-line distance is the
    // real approach distance (slopes/ramps stay under it), so trust the gate
    // above. Skips the path probe in the overwhelmingly common case.
    float const dz = std::fabs(bz - bot->GetPositionZ());
    if (dz <= DC_Z_LEVEL_TOLERANCE)
        return true;

    // Static coords (boss not loaded) are necessarily far and only reached on
    // the tank's own floor; no under/over-the-ledge case to guard against.
    if (!live)
        return true;

    // Vertically separated: a 3D-close boss may be a whole floor below or above
    // — the tank on a balcony/walkway directly over Rethilgore, or parked under
    // an upper-floor boss like Fenrus. Straight-line proximity is then a lie:
    // the real approach winds down stairs/ramps and the boss can neither be
    // pulled (no LOS / out of its own aggro band) nor does the tank ever reach
    // it, so the at-boss handoff fires and the run dead-stops in "Holding near".
    //
    // The old guard (IsLevelReachable) only asked whether SOME ground path to
    // the boss's level exists — which, in a connected dungeon, it almost always
    // does — so it failed to catch this. Require instead that the NAVIGATIONAL
    // distance to the boss is itself within engage range: a real arrival, not a
    // long detour down. A boss directly overhead clamps off-mesh (PathGenerator
    // snaps the destination back to the tank's own floor) and fails the
    // end-on-boss-level check; a balcony boss produces a long stairs path and
    // fails the length check. Either way the handoff defers and Advance keeps
    // walking the route to the boss's floor, where dz collapses and the pull
    // fires for real.
    PathGenerator gen(bot);
    gen.CalculatePath(bx, by, bz, /*forceDest*/ false);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;

    Movement::PointsArray const& path = gen.GetPath();
    if (path.empty())
        return false;
    if (std::fabs(path.back().z - bz) > DC_Z_LEVEL_TOLERANCE)
        return false;

    return gen.getPathLength() <= engageRange;
}

bool DungeonClearUtil::IsDoorClosed(GameObject const* go)
{
    if (!go || !go->IsInWorld())
        return false;
    GameObjectTemplate const* info = go->GetGOInfo();
    if (!info || info->type != GAMEOBJECT_TYPE_DOOR)
        return false;
    // Authored non-blocking (decorative / always-passable) doors never count.
    if (info->door.ignoredByPathing)
        return false;
    // The GOState->open/closed mapping is inverted by the door.startOpen
    // template flag: a normal door (startOpen=0) is closed at GO_STATE_READY,
    // but a gate that spawns open (startOpen=1) also sits at GO_STATE_READY
    // while appearing OPEN. So closed iff GO_STATE_READY xor startOpen.
    bool const startOpen = info->door.startOpen != 0;
    return (go->GetGoState() == GO_STATE_READY) != startOpen;
}

bool DungeonClearUtil::ClosedDoorBetween(Player* bot, float tx, float ty,
                                         float tz, float corridorWidth)
{
    if (!bot)
        return false;
    Map* map = bot->GetMap();
    if (!map)
        return false;

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    float const bz = bot->GetPositionZ();
    float const widthSq = corridorWidth * corridorWidth;
    // Floor span of the bot->target line (plus tolerance). A door outside it
    // sits on another level and isn't actually in the way (stacked decks /
    // ramps), even though its 2D origin lands near this straight chord.
    float const loZ = std::min(bz, tz) - DC_DOOR_Z_BAND;
    float const hiZ = std::max(bz, tz) + DC_DOOR_Z_BAND;

    float const segLenSq = (tx - bx) * (tx - bx) + (ty - by) * (ty - by);

    for (auto const& kv : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = kv.second;
        if (!IsDoorClosed(go))
            continue;

        float const gx = go->GetPositionX();
        float const gy = go->GetPositionY();
        float const gz = go->GetPositionZ();

        // Different floor — near in plan-view but not on the way.
        if (gz < loZ || gz > hiZ)
            continue;

        // Within corridorWidth of the bot->target segment?
        float const d2 =
            DungeonClearMath::DistSqToSegment2D(gx, gy, bx, by, tx, ty);
        if (d2 > widthSq)
            continue;

        // Require the door to project to the INTERIOR of the chord, not be
        // clamped to an endpoint. A door beside the bot or beside the target
        // (a parallel corridor the straight chord grazes near an end) is not
        // "between" us and the target — only a door the chord passes THROUGH
        // is. Without this the up-to-35yd trash chord, which cuts across walls
        // on a winding route, vetoed valid near-side packs. A door genuinely in
        // the doorway still lands interior. (Degenerate zero-length chord: any
        // door within band counts, as before.)
        if (segLenSq > 0.01f)
        {
            float const t =
                ((gx - bx) * (tx - bx) + (gy - by) * (ty - by)) / segLenSq;
            if (t <= 0.05f || t >= 0.95f)
                continue;
        }

        return true;
    }
    return false;
}

float DungeonClearUtil::DistAlongPathToClosedDoor(
    Player* bot, ChunkedPathfinder::Result const& path,
    float doorX, float doorY, float doorZ, float maxLookAhead)
{
    if (!bot || !path.reachable || path.segments.empty())
        return std::numeric_limits<float>::max();

    // Walk the smoothed polyline accumulating distance FROM THE PATH START, and
    // track two cursors: where the path is closest to the bot (the tank's current
    // progress along it) and where it first enters THIS door's band. Return the
    // forward gap between them — the travel still REMAINING to the doorway.
    //
    // The accumulate-from-the-bot version was wrong: the long-path is anchored
    // where it was built, which falls behind as the tank advances, so walking
    // from the bot counted the already-walked prefix and the returned distance
    // GREW as the tank closed in (it never dropped to the stand-off until the bot
    // physically entered the band, parking it on the door). Subtracting the bot's
    // progress cursor fixes that. Only this one door is tested: scanning every
    // nearby door returned whichever the winding route grazed first (an off-route
    // side door), masking the real blocker.
    float const bandSq = DC_DOOR_BAND * DC_DOOR_BAND;
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    float const zBand = DC_DOOR_Z_BAND;
    float prevX = 0.0f, prevY = 0.0f, prevZ = 0.0f;
    bool havePrev = false;
    float accumulated = 0.0f;     // distance from path start to current point
    float cursorAccum = 0.0f;     // accumulated at the point closest to the bot
    float bestBotDistSq = std::numeric_limits<float>::max();
    float doorAccum = -1.0f;      // accumulated at the band entry

    auto visit = [&](float px, float py, float pz) -> bool
    {
        if (havePrev)
        {
            // Only treat the door as on THIS leg if the leg shares its floor —
            // otherwise the route passing over/under the door (a stacked deck or
            // a ramp) registers a band entry far before the real doorway and
            // parks the tank short.
            bool const onFloor =
                doorZ >= std::min(prevZ, pz) - zBand &&
                doorZ <= std::max(prevZ, pz) + zBand;
            if (onFloor &&
                DungeonClearMath::DistSqToSegment2D(doorX, doorY, prevX, prevY, px, py) <= bandSq)
            {
                doorAccum = accumulated;   // at the START (prev) of the hitting segment
                return true;
            }
            float const dx = px - prevX;
            float const dy = py - prevY;
            accumulated += std::sqrt(dx * dx + dy * dy);
        }
        float const bdx = px - botX;
        float const bdy = py - botY;
        float const bd2 = bdx * bdx + bdy * bdy;
        if (bd2 < bestBotDistSq)
        {
            bestBotDistSq = bd2;
            cursorAccum = accumulated;
        }
        prevX = px;
        prevY = py;
        prevZ = pz;
        havePrev = true;
        return false;
    };

    bool hit = false;
    for (PathSegment const& seg : path.segments)
    {
        if (seg.polyline.empty())
            hit = visit(seg.ex, seg.ey, seg.ez);
        else
            for (G3D::Vector3 const& pt : seg.polyline)
                if ((hit = visit(pt.x, pt.y, pt.z)))
                    break;
        if (hit || accumulated >= maxLookAhead)
            break;
    }

    if (!hit)
        return std::numeric_limits<float>::max();

    return std::max(0.0f, doorAccum - cursorAccum);
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

    // Within DC_Z_LEVEL_TOLERANCE the candidate is on the bot's own level:
    // slopes, stairs and ramps stay under it within a corridor lookahead, so
    // we trust the caller's 2D corridor/cone/LOS test and skip the probe.
    // Otherwise a genuine other-level mob falls through to the pathfinder
    // check below.
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

    // Truncate the scanned corridor at the first closed door so trash on the FAR
    // side is never picked: door-blind, the corridor runs straight through doors
    // the navmesh doesn't model as solid, and engage-trash (25) would beat
    // door-blocked (22) and walk the tank through to the pack. Computed fresh from
    // the live door state here (not the 500ms-cached blocking-door value), so it
    // holds even on the tick a scan first sees the pack.
    auto const doors =
        CollectClosedDoors(bot->GetMap(), bot->GetPositionX(), bot->GetPositionY(),
                           maxLookAhead + DC_DOOR_BAND);

    float prevX = bot->GetPositionX();
    float prevY = bot->GetPositionY();
    float prevZ = bot->GetPositionZ();
    float accumulated = 0.0f;
    bool stop = false;
    for (PathSegment const& seg : segments)
    {
        // Anchored segments collapse to a single polyline point; non-anchored
        // segments carry the full smoothed corridor. Fall back to the endpoint
        // only if a segment somehow has no polyline at all.
        if (seg.polyline.empty())
        {
            Seg2D const s{prevX, prevY, seg.ex, seg.ey};
            if (!doors.empty() && SegmentHitsClosedDoor(s, prevZ, seg.ez, doors))
                break;
            float const dx = seg.ex - prevX;
            float const dy = seg.ey - prevY;
            segs.push_back(s);
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = seg.ex;
            prevY = seg.ey;
            prevZ = seg.ez;
            if (accumulated >= maxLookAhead)
                break;
            continue;
        }

        for (G3D::Vector3 const& pt : seg.polyline)
        {
            Seg2D const s{prevX, prevY, pt.x, pt.y};
            if (!doors.empty() && SegmentHitsClosedDoor(s, prevZ, pt.z, doors))
            {
                stop = true;
                break;
            }
            float const dx = pt.x - prevX;
            float const dy = pt.y - prevY;
            segs.push_back(s);
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = pt.x;
            prevY = pt.y;
            prevZ = pt.z;
            if (accumulated >= maxLookAhead)
            {
                stop = true;
                break;
            }
        }
        if (stop)
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

float DungeonClearUtil::RestMinHpPct(Player* bot)
{
    // Per-run override wins: the group set a health rest target for this run, and
    // DungeonClearNeedsEatTrigger makes bots actually eat up to it, so we use it
    // verbatim (no playerbots clamp — even a target above AlmostFullHealth is
    // reachable now). 0 means "inherit" and falls through.
    if (uint32 const target = DcSettings::GetUInt(bot, "RestHealthPct"))
        return static_cast<float>(target);

    // 90% is our "topped up enough to pull" ceiling. Clamp it to the level bots
    // actually eat back up to (AiPlayerbot.AlmostFullHealth, default 85) so the
    // gate never waits on HP a bot won't restore on its own.
    return std::min(90.0f, static_cast<float>(sPlayerbotAIConfig.almostFullHealth));
}

float DungeonClearUtil::RestMinMpPct(Player* bot)
{
    // Per-run override wins; see RestMinHpPct. DungeonClearNeedsDrinkTrigger makes
    // bots drink up to this target, so it stays reachable above HighMana too.
    if (uint32 const target = DcSettings::GetUInt(bot, "RestManaPct"))
        return static_cast<float>(target);

    // 75% ceiling, clamped to the level bots actually drink back up to
    // (AiPlayerbot.HighMana, default 65). Bots stop drinking at HighMana, so a
    // higher gate would strand the tank waiting on slow natural mana regen.
    return std::min(75.0f, static_cast<float>(sPlayerbotAIConfig.highMana));
}

Player* DungeonClearUtil::FindLeaderTank(Player* reference)
{
    if (!reference)
        return nullptr;

    Group* group = reference->GetGroup();
    if (!group)
    {
        // Solo: a tank bot leads itself; anyone else has no leader.
        return (PlayerbotAI::IsTank(reference) && GET_PLAYERBOT_AI(reference))
                   ? reference : nullptr;
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
    return leader;
}

bool DungeonClearUtil::IsDungeonClearLeader(Player* bot)
{
    return bot && FindLeaderTank(bot) == bot;
}

bool DungeonClearUtil::IsInPausedDungeonClearRun(Player* bot)
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

bool DungeonClearUtil::IsPullPhaseHolding(uint32 phase)
{
    return phase == static_cast<uint32>(DcPullPhase::Forming) ||
           phase == static_cast<uint32>(DcPullPhase::Advancing) ||
           phase == static_cast<uint32>(DcPullPhase::Returning);
}

std::optional<Position> DungeonClearUtil::ComputeSafeCamp(PlayerbotAI* botAI, Unit* target,
                                                          float setback, float safeRadius,
                                                          float maxDrag,
                                                          float& clearanceOut, float& dragOut)
{
    clearanceOut = std::numeric_limits<float>::max();
    dragOut = 0.0f;
    if (!botAI || !target)
        return std::nullopt;
    Player* bot = botAI->GetBot();
    if (!bot)
        return std::nullopt;
    AiObjectContext* ctx = botAI->GetAiObjectContext();

    // Grouping radius: hostiles this close to the pulled target are its own
    // packmates — they come along to camp anyway, so they don't count against
    // camp clearance. Everything farther is an "other" pack we must stay clear of.
    constexpr float kPackRadius = 12.0f;
    // How far apart to sample candidate camp points walking back along the route.
    constexpr float kStep = 4.0f;
    if (maxDrag < setback)
        maxDrag = setback;

    // Resolve the other-pack hostiles once (alive, hostile, not the target, not a
    // packmate). Same candidate set the pull / trash scans use.
    GuidVector const& farTargets =
        ctx->GetValue<GuidVector>("dungeon clear far targets")->Get();
    GuidVector const& possibleTargets =
        ctx->GetValue<GuidVector>("possible targets")->Get();
    GuidVector const& candidates = farTargets.empty() ? possibleTargets : farTargets;

    std::vector<Unit*> others;
    others.reserve(candidates.size());
    for (ObjectGuid guid : candidates)
    {
        Unit* u = ObjectAccessor::GetUnit(*bot, guid);
        if (!u || !u->IsAlive() || u == target)
            continue;
        if (!bot->IsHostileTo(u))
            continue;
        if (u->GetExactDist2d(target) <= kPackRadius)
            continue;  // packmate — fought regardless of where camp sits
        others.push_back(u);
    }

    auto clearanceAt = [&others](Position const& p) -> float
    {
        float nearest = std::numeric_limits<float>::max();
        for (Unit* u : others)
            nearest = std::min(nearest, p.GetExactDist2d(u));
        return nearest;
    };

    std::vector<Position> const& crumbs =
        ctx->GetValue<std::vector<Position>&>("dungeon clear breadcrumbs")->Get();

    Position const tankPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    // --- Preferred: walk BACK along the breadcrumb trail ------------------
    // The trail is the ground the tank actually walked (oldest -> newest), so it
    // stays valid even though the pull drag-back resets the long-path cursor (the
    // bug that made a cursor-based "point behind" find only a few yards after the
    // first pull). Accumulate corridor distance from the tank backward through it.
    // Dungeon mobs have no leash, so take the first point at least `setback` back
    // that also clears safeRadius, walking further (up to maxDrag) only if a
    // neighbour is still within safeRadius. A gap bigger than kJumpGuard means the
    // trail isn't contiguous there (a drag/teleport boundary) — stop, nothing
    // beyond it is really "behind us". Track the farthest point as the fallback.
    constexpr float kJumpGuard = 12.0f;
    Position best = tankPos;
    float bestClear = clearanceAt(tankPos);
    float bestDrag = 0.0f;
    float bestAlong = 0.0f;
    Position prev = tankPos;
    float along = 0.0f;
    for (std::size_t i = crumbs.size(); i-- > 0; )
    {
        Position const& c = crumbs[i];
        float const seg = prev.GetExactDist2d(&c);
        prev = c;
        if (seg > kJumpGuard)
            break;  // discontinuity behind us — stop here
        along += seg;
        float const clear = clearanceAt(c);
        float const drag = tankPos.GetExactDist2d(&c);
        if (along > bestAlong)  // farthest back so far (fallback)
        {
            best = c;
            bestClear = clear;
            bestDrag = drag;
            bestAlong = along;
        }
        if (along >= setback && clear >= safeRadius)
        {
            clearanceOut = clear;
            dragOut = drag;
            return c;
        }
        if (along >= maxDrag)
            break;
    }

    // Got a meaningful distance back along the trail (at least half the setback):
    // use the farthest point even if a neighbour is within safeRadius — no leash,
    // so far-and-imperfect beats close.
    if (bestAlong >= setback * 0.5f)
    {
        clearanceOut = bestClear;
        dragOut = bestDrag;
        return best;
    }

    // --- Fallback: cleared route behind is too short (e.g. first pull) ------
    // Pull straight back, directly away from the target, snapped to the navmesh.
    // Try the full setback first, shrinking until a walkable point is found.
    float dx = tankPos.GetPositionX() - target->GetPositionX();
    float dy = tankPos.GetPositionY() - target->GetPositionY();
    float const len = std::sqrt(dx * dx + dy * dy);
    if (len > 0.1f)
    {
        dx /= len;
        dy /= len;
        for (float d = setback; d >= kStep; d -= kStep)
        {
            NavmeshSnap::Result snap = NavmeshSnap::Snap(
                bot, tankPos.GetPositionX() + dx * d,
                tankPos.GetPositionY() + dy * d, tankPos.GetPositionZ(), 10.0f);
            if (!snap.ok)
                continue;
            Position cand(snap.x, snap.y, snap.z);
            float const c = clearanceAt(cand);
            float const drag = tankPos.GetExactDist2d(&cand);
            if (drag > bestDrag)
            {
                best = cand;
                bestClear = c;
                bestDrag = drag;
            }
            if (c >= safeRadius)
            {
                clearanceOut = c;
                dragOut = drag;
                return cand;
            }
        }
    }

    clearanceOut = bestClear;
    dragOut = bestDrag;
    return best;
}

bool DungeonClearUtil::IsPartySetAtCamp(Player* leader, Position const& camp, float setRadius)
{
    if (!leader)
        return false;
    Group* group = leader->GetGroup();
    if (!group)
        return true;  // solo tank — nobody to wait on

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == leader)
            continue;
        if (member->GetMapId() != leader->GetMapId())
            continue;
        if (member->isDead())
            continue;  // dead members handled by the party-died trigger
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;  // real player — never gate the pull on them
        if (member->GetExactDist2d(&camp) > setRadius)
            return false;
        if (!memberAI->HasStrategy("passive", BOT_STATE_COMBAT))
            return false;
    }
    return true;
}

bool DungeonClearUtil::GetLeaderPullInfo(Player* bot, uint32& phaseOut, Position& campOut)
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
    // Pull behavior only matters while the run is live, unpaused, and pull mode
    // is on. A paused/off leader holds the whole party via the normal gates.
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        ctx->GetValue<bool>("dungeon clear paused")->Get() ||
        !ctx->GetValue<bool>("dungeon clear pull mode")->Get())
        return false;

    uint32 const phase = ctx->GetValue<uint32>("dungeon clear pull phase")->Get();
    if (phase == static_cast<uint32>(DcPullPhase::Idle))
        return false;

    phaseOut = phase;
    campOut = ctx->GetValue<Position&>("dungeon clear camp position")->Get();
    return true;
}

bool DungeonClearUtil::GetLeaderCampHold(Player* bot, Position& campOut, bool& passiveOut)
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
        ctx->GetValue<bool>("dungeon clear paused")->Get() ||
        !ctx->GetValue<bool>("dungeon clear pull mode")->Get())
        return false;

    Position const camp = ctx->GetValue<Position&>("dungeon clear camp position")->Get();
    // No camp marked yet (pull mode just toggled on, or a reset cleared it): there
    // is nothing to hold at, so let the caller fall back (briefly) to follow.
    if (camp.GetPositionX() == 0.0f && camp.GetPositionY() == 0.0f &&
        camp.GetPositionZ() == 0.0f)
        return false;

    uint32 const phase = ctx->GetValue<uint32>("dungeon clear pull phase")->Get();
    campOut = camp;
    passiveOut = IsPullPhaseHolding(phase);
    return true;
}

void DungeonClearUtil::AbortLeaderPull(Player* bot)
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
    if (IsPullPhaseHolding(ctx->GetValue<uint32>("dungeon clear pull phase")->Get()))
    {
        ctx->GetValue<uint32>("dungeon clear pull phase")
            ->Set(static_cast<uint32>(DcPullPhase::Engage));
        DC_PULL_INFO("[DC:{}] advanced-pull: leader pull aborted (forced to Engage) "
                     "-> party released", leader->GetName());
    }
}

void DungeonClearUtil::SetLeaderDazeImmunity(Player* leader, bool apply)
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
        // Sticky entries (permanent skip reason — empty / below-floor / un-
        // takeable by nature) never expire; only DisableDungeonClear clears them.
        if (it->second != LOOT_SKIP_STICKY && now >= it->second)
        {
            // Expired — drop it so a residual transient block can be retried
            // once (a second looter who has since left the corpse, own loot
            // we couldn't reach in time). NOT for group rolls: a roll we lose
            // is gone for us and a roll we win auto-delivers, so roll-locked
            // loot is recorded sticky upstream, never here.
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
    if (ttlMs == LOOT_SKIP_STICKY)
    {
        skip[guid] = LOOT_SKIP_STICKY;  // never expires (permanent skip reason)
        return;
    }
    // A real ttl: store the expiry. Guard the one-tick window every ~49 days
    // where getMSTime() + ttlMs wraps to exactly the sticky sentinel — bump it
    // off 0 so a transient skip is never mistaken for a permanent one.
    uint32 expiry = getMSTime() + ttlMs;
    if (expiry == LOOT_SKIP_STICKY)
        expiry = 1u;
    skip[guid] = expiry;
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

bool DungeonClearUtil::MaybeSkipUnworthyLoot(PlayerbotAI* botAI)
{
    if (!botAI)
        return false;

    Player* bot = botAI->GetBot();
    AiObjectContext* ctx = botAI->GetAiObjectContext();

    uint32 const minQuality = DcSettings::GetUInt(bot, "LootMinQuality");
    // When set, the tank never stops for chests (or any other world object) —
    // only creature corpses are worth a detour. Default on: chests routinely
    // sat off the route and snared the navmesh approach. See DungeonClear.conf.
    bool const ignoreChests = DcSettings::GetBool(bot, "IgnoreChests");

    // Drain EVERY in-range unworthy corpse this tick, not just the single
    // nearest. Each pass judges the loot the bot would commit to next — stock's
    // chosen target if set, else the nearest in the stack (the same selection
    // GiveUpCurrentLoot blacklists, so the two stay aligned). When that corpse
    // is below the floor, we blacklist + strip it so the loot flags drop for it,
    // then re-evaluate the now-nearest remaining corpse and repeat.
    //
    // Judging only one per tick (the old behavior) made the tank stutter when
    // backtracking through a field of skipped corpses: it stripped one corpse
    // but "has available loot" stayed true for the rest, so the advance loot-
    // yield halted it (StopMoving) for as many ticks as there were corpses. By
    // clearing them all in a single tick the tank never stops for loot it won't
    // take — it walks straight through, held only by the party-spread gate. The
    // loop terminates because every skip removes the corpse from the stack (via
    // StripSkippedLoot), so GetLoot can't return it again; the cap is a belt-
    // and-braces guard against an unexpectedly large cluster.
    bool skippedAny = false;
    constexpr int kMaxDrain = 32;
    for (int i = 0; i < kMaxDrain; ++i)
    {
        LootObject target = ctx->GetValue<LootObject>("loot target")->Get();
        if (target.guid.IsEmpty())
            if (LootObjectStack* stack = ctx->GetValue<LootObjectStack*>("available loot")->Get())
                target = stack->GetLoot(sPlayerbotAIConfig.lootDistance);
        if (target.guid.IsEmpty())
            break;  // nothing left to judge

        // Classify the next pickup. Dungeon-clear stops for creature CORPSES
        // that hold loot we'd take, and (only when IgnoreChests is off) for
        // genuine treasure CHESTS; everything else is skipped so the bot never
        // detours onto it.
        bool keep = false;
        if (Creature* creature = botAI->GetCreature(target.guid))
        {
            // A corpse. Worth a stop only if it carries loot this bot can
            // actually take (above the quality floor, not locked in someone
            // else's roll). A skinnable-only corpse has no normal loot here, so
            // CorpseHasTakeableLoot is false and it is skipped like a gathering
            // node — the bot does not stop merely to skin.
            keep = CorpseHasTakeableLoot(bot, creature, minQuality);
        }
        else if (GameObject* go = botAI->GetGameObject(target.guid))
        {
            // A gameobject. With IgnoreChests on (the default) no world object
            // is ever worth a detour — the bot stops only for corpses. With it
            // off, stop only for real chests. Herbalism / mining gathering veins
            // are also chest-type gameobjects, but are gated by a profession-
            // skill lock — skillId carries that profession — so exclude them;
            // every non-chest gameobject (fishing hole, lever, quest object) is
            // excluded by type.
            keep = !ignoreChests && go->GetGoType() == GAMEOBJECT_TYPE_CHEST &&
                   target.skillId != SKILL_HERBALISM && target.skillId != SKILL_MINING;
        }
        // else: loose item loot or an unresolvable guid -> not a corpse or chest.

        if (keep)
            break;  // nearest is worth a stop -> let the loot pipeline run

        // Not a corpse-with-loot or a chest -> blacklist + strip now so the loot
        // flags drop this tick and the bot skips the detour entirely (the
        // proactive analogue of the camp/yield timeouts firing after a wasted
        // walk). Sticky: every reason we get here is permanent for the run, so
        // the corpse must never re-arm the yield on a later backtrack.
        GiveUpCurrentLoot(botAI, LOOT_SKIP_STICKY);
        StripSkippedLoot(botAI);
        skippedAny = true;
    }
    return skippedAny;
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

namespace
{
    // GUIDs of players that currently carry a DC follow-tank MoveFollow
    // generator. Mutated from the follow-tank action (bot AI update) and read by
    // the reaper (world update / OnPlayerbotUpdate); both run on the world
    // thread today, but the set is tiny and the lock is uncontended, so guard it
    // anyway to stay correct if bot updates ever move off-thread.
    std::set<ObjectGuid> g_dcFollowingPlayers;
    std::mutex g_dcFollowingMutex;

    // GUIDs of followers DC has put into the mod-playerbots "passive" combat
    // strategy for an advanced pull. Tracked so ReapStrandedPassives is the
    // single authoritative teardown — it removes passive the moment the leader
    // leaves a holding pull phase, even for a follower that got dragged into
    // combat (its own engines can't self-heal a passive-lock mid-fight). Only
    // passive DC itself applied lives here; a player's manual passive is never
    // recorded, so it's never clobbered.
    std::set<ObjectGuid> g_dcPassivePlayers;
    std::mutex g_dcPassiveMutex;
}

void DungeonClearUtil::MarkFollowing(ObjectGuid player)
{
    std::lock_guard<std::mutex> lock(g_dcFollowingMutex);
    g_dcFollowingPlayers.insert(player);
}

void DungeonClearUtil::UnmarkFollowing(ObjectGuid player)
{
    std::lock_guard<std::mutex> lock(g_dcFollowingMutex);
    g_dcFollowingPlayers.erase(player);
}

void DungeonClearUtil::ReapOrphanedFollows()
{
    std::lock_guard<std::mutex> lock(g_dcFollowingMutex);
    if (g_dcFollowingPlayers.empty())
        return;

    for (auto it = g_dcFollowingPlayers.begin(); it != g_dcFollowingPlayers.end();)
    {
        Player* player = ObjectAccessor::FindPlayer(*it);

        // Player left the world (logged out, bot despawned): nothing to clear,
        // and the GUID would otherwise linger forever. Drop it.
        if (!player || !player->IsInWorld())
        {
            it = g_dcFollowingPlayers.erase(it);
            continue;
        }

        // AI still ticking -> its own follow-tank teardown owns this generator;
        // leave the mark in place and move on.
        if (GET_PLAYERBOT_AI(player))
        {
            ++it;
            continue;
        }

        // AI gone but the player is still in world (a self-bot toggled out of bot
        // mode). Cancel the leftover continuous follow so movement control reverts
        // to the human; a real player has no AI to self-heal it otherwise.
        if (player->GetMotionMaster() &&
            player->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            if (player->isMoving())
                player->StopMoving();
            player->GetMotionMaster()->Clear();
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] reaped orphaned follow generator (self-bot left bot "
                     "mode) -> movement control returned to player",
                     player->GetName());
        }

        it = g_dcFollowingPlayers.erase(it);
    }
}

void DungeonClearUtil::ApplyFollowerPassive(Player* follower)
{
    if (!follower)
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(follower);
    if (!botAI)
        return;

    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        // Already DC-managed — nothing to do (idempotent across hold ticks).
        if (g_dcPassivePlayers.count(follower->GetGUID()))
            return;
    }

    // A passive the player set themselves is left entirely alone: don't add a
    // duplicate, and don't record it (so we never strip it on release).
    if (botAI->HasStrategy("passive", BOT_STATE_COMBAT))
        return;

    botAI->ChangeStrategy("+passive", BOT_STATE_COMBAT);
    if (Pet* pet = follower->GetPet())
        pet->SetReactState(REACT_PASSIVE);

    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        g_dcPassivePlayers.insert(follower->GetGUID());
    }

    DC_PULL_DEBUG("[DC:{}] advanced-pull: held passive at camp", follower->GetName());
}

void DungeonClearUtil::RemoveFollowerPassive(Player* follower)
{
    if (!follower)
        return;

    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        // Only strip passive we applied; a manual passive was never recorded.
        if (!g_dcPassivePlayers.erase(follower->GetGUID()))
            return;
    }

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(follower))
    {
        if (botAI->HasStrategy("passive", BOT_STATE_COMBAT))
            botAI->ChangeStrategy("-passive", BOT_STATE_COMBAT);
        if (Pet* pet = follower->GetPet())
            pet->SetReactState(REACT_DEFENSIVE);

        DC_PULL_DEBUG("[DC:{}] advanced-pull: released from passive", follower->GetName());
    }
}

void DungeonClearUtil::ReapStrandedPassives()
{
    std::vector<ObjectGuid> marked;
    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        if (g_dcPassivePlayers.empty())
            return;
        marked.assign(g_dcPassivePlayers.begin(), g_dcPassivePlayers.end());
    }

    float const safetyHp = sConfigMgr->GetOption<float>("DungeonClear.PullSafetyHpPct", 50.0f);

    for (ObjectGuid guid : marked)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);

        // Player gone, or its AI was deleted (self-bot left bot mode): we can no
        // longer drive a ChangeStrategy, so just drop the mark. The strategy was
        // never persisted to the DB (direct ChangeStrategy doesn't Save), so it
        // evaporates with the engine anyway.
        if (!player || !player->IsInWorld() || !GET_PLAYERBOT_AI(player))
        {
            std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
            g_dcPassivePlayers.erase(guid);
            continue;
        }

        uint32 phase = 0;
        Position camp;
        bool const inPull = GetLeaderPullInfo(player, phase, camp);

        // Release this follower the moment the leader leaves a holding phase.
        // GetLeaderPullInfo returns true for Engage too (only Idle returns
        // false), so a plain !inPull test would keep the party passive through
        // the entire camp fight: the tank flips to Engage on arrival but only
        // drops to Idle out of combat — which never happens while it's tanking.
        // Gate on IsPullPhaseHolding instead so Engage releases the party (per
        // the DcPullPhase contract: "Idle and Engage release them"), along with
        // pull off / dc off / paused / leader gone. The single authoritative
        // teardown — fires regardless of the follower's own engine state.
        if (!inPull || !IsPullPhaseHolding(phase))
        {
            RemoveFollowerPassive(player);
            continue;
        }

        // Camp-safety valve: a held, passive follower taking real damage (a
        // patrol clipped camp, or the pull went sideways) can't defend itself —
        // abort the pull so the whole party drops passive and fights back.
        if (player->IsInCombat() && safetyHp > 0.0f &&
            player->GetHealthPct() < safetyHp)
        {
            DC_PULL_INFO("[DC:{}] advanced-pull SAFETY: passive follower at {:.0f}% in "
                         "combat -> aborting pull, releasing party",
                         player->GetName(), player->GetHealthPct());
            AbortLeaderPull(player);
        }
    }
}

// ---------------------------------------------------------------------------
// Event-driven STATUS / BOSS pushes
// ---------------------------------------------------------------------------
//
// Rather than have the addon poll `CMD\tstatus` on a fixed interval, the server
// recomputes the status string cheaply each world tick for the small set of
// tanks actually running a clear and emits a packet only when the meaningful
// state changes. BuildStatusPayload is the single source of truth for the
// STATUS line (also used by the on-demand `dc status`), so the two paths can
// never diverge.

namespace
{
    // Per-tank push bookkeeping. lastStatus is the exact STATUS payload last
    // sent (the change-detector compares against it verbatim); lastBossSig is a
    // cheap fingerprint of the boss list so the expensive boss re-scan only runs
    // when a boss actually dies / gets skipped / the target changes. `primed`
    // guards the very first tick so a freshly-marked tank always emits once.
    struct DcPushState
    {
        std::string lastStatus;
        uint64 lastBossSig = 0;
        bool primed = false;
    };

    std::map<ObjectGuid, DcPushState> g_dcActiveTanks;
    std::mutex g_dcActiveTanksMutex;

    // Throttle accumulator for the world-tick detector (ms).
    uint32 g_dcPushAccumMs = 0;
    constexpr uint32 DC_PUSH_INTERVAL_MS = 400;

    // Cheap fingerprint of the boss list's user-visible state. Driven entirely
    // off values that are O(1) to read — the completed-encounter bitmask (flips
    // when a boss dies), the skipped-set size, and the committed target — so it
    // can be checked every detector pass without the per-boss creature scans
    // that BuildBossList / DcBossesAction perform. alive->missing transitions
    // are intentionally NOT captured here (they need the scan and are cosmetic);
    // the addon still refreshes those via its zone-change / window-show request.
    uint64 BuildBossSignature(PlayerbotAI* botAI)
    {
        AiObjectContext* context = botAI->GetAiObjectContext();
        Player* bot = botAI->GetBot();
        if (!bot)
            return 0;

        InstanceScript* inst = DungeonClearUtil::GetInstanceScript(bot);
        uint32 const mask = inst ? inst->GetCompletedEncounterMask() : 0u;
        uint32 const skipped =
            static_cast<uint32>(AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped").size());
        uint32 const selected = AI_VALUE(uint32, "dungeon clear selected boss");
        uint32 const count =
            static_cast<uint32>(AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses").size());

        // Fold in the map + instance id so the detector always fires when the
        // tank crosses into a different instance (clear one dungeon, walk into
        // the next; or step from a dungeon into the raid it gates). Without it a
        // transition where {mask,skipped,selected,count} happen to coincide
        // would leave the addon showing the prior dungeon's bosses.
        uint32 const mapId = bot->GetMapId();
        uint32 const instanceId = bot->GetInstanceId();

        // FNV-1a over the words. Collisions are harmless: the only cost of
        // a missed change is a slightly stale boss row until the next real
        // transition or an explicit request.
        uint64 h = 1469598103934665603ull;
        for (uint32 w : {mask, skipped, selected, count, mapId, instanceId})
        {
            h ^= w;
            h *= 1099511628211ull;
        }
        return h;
    }
}

std::string DungeonClearUtil::BuildStatusPayload(PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    Player* bot = botAI->GetBot();

    bool const enabled = AI_VALUE(bool, "dungeon clear enabled");
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    auto const& skipped = AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped");
    std::string const& stall = AI_VALUE(std::string&, "dungeon clear stall reason");

    // Calculate dynamic state for addon UI. Authoritative conditions (combat,
    // stall, loot, rest) take precedence over the advance action's self-reported
    // navigation phase, since they reflect ground truth the phase token can't
    // see. `detail` is a short human sentence the addon shows under the state
    // line — who we're waiting on, what we're heading to, etc.
    std::string stateStr = "off";
    std::string detail;
    bool const paused = AI_VALUE(bool, "dungeon clear paused");
    bool const pullMode = AI_VALUE(bool, "dungeon clear pull mode");
    uint32 const pullPhase = AI_VALUE(uint32, "dungeon clear pull phase");
    std::string const bossName = next.has_value() ? next->name : "the boss";

    if (enabled && paused)
    {
        // Paused takes precedence over every running sub-state — the addon
        // paints this state yellow. `enabled` stays 1 so the addon can see the
        // eventual resume. `detail` carries WHY we're paused (set at the pause
        // site: a manual hold vs. a door the tank can't open) so the panel can
        // report the cause; the addon supplies the "boss progress saved"
        // reassurance line. Fall back to a generic hold if no reason was stamped.
        stateStr = "paused";
        std::string const& pauseReason = AI_VALUE(std::string&, "dungeon clear pause reason");
        detail = pauseReason.empty() ? "holding position" : pauseReason;
    }
    else if (enabled && bot && pullMode && DungeonClearUtil::IsPullPhaseHolding(pullPhase))
    {
        // Mid advanced-pull. Takes precedence over the combat sub-state — during
        // the return leg the tank is in combat but we want to report the pull.
        stateStr = "pulling";
        if (pullPhase == static_cast<uint32>(DcPullPhase::Returning))
            detail = "Pulling the pack back to camp.";
        else
            detail = "Pulling — party holding at camp.";
    }
    else if (enabled && bot)
    {
        if (bot->IsInCombat())
        {
            Unit* currentTarget = context->GetValue<Unit*>("current target")->Get();
            if (currentTarget && next.has_value() && currentTarget->GetEntry() == next->entry)
            {
                stateStr = "fighting_boss";
                detail = "Engaging " + bossName + ".";
            }
            else
            {
                stateStr = "fighting_trash";
                detail = (currentTarget && !currentTarget->GetName().empty())
                             ? ("Fighting " + currentTarget->GetName() + ".")
                             : "Clearing trash from the path.";
            }
        }
        else if (!stall.empty())
        {
            ObjectGuid door = context->GetValue<ObjectGuid>("dungeon clear blocking door")->Get();
            stateStr = door.IsEmpty() ? "stalled" : "door_blocked";
            // The stall reason already rides in its own field; leave detail empty.
        }
        else if (AI_VALUE(bool, "has available loot") || AI_VALUE(bool, "can loot") ||
                 DungeonClearUtil::IsAnyPartyMemberLooting(bot))
        {
            stateStr = "looting";
            std::string const who = DungeonClearUtil::DescribePartyLooting(bot);
            detail = who.empty() ? "Collecting loot." : (who + ".");
        }
        // Status display must use the SAME spread the advance gate enforces
        // (DungeonClear.PartyMaxSpread), or the tank parks at the limit while
        // still reporting "En route" instead of "Waiting on". In advanced-pull
        // mode the advance gate (IsBetweenPullsReady) drops the spread check —
        // the party deliberately holds at camp while the tank scouts ahead — so
        // mirror that here, or the panel falsely reports "Waiting on … (out of
        // range)" while the tank is actually still moving out to pull.
        else if (float const maxSpread = pullMode
                     ? 100000.0f
                     : sConfigMgr->GetOption<float>("DungeonClear.PartyMaxSpread", 25.0f);
                 !DungeonClearUtil::IsPartyReady(bot, RestMinHpPct(bot), RestMinMpPct(bot), maxSpread))
        {
            stateStr = "resting";
            std::string const who = DungeonClearUtil::DescribePartyNotReady(bot, RestMinHpPct(bot),
                                                                            RestMinMpPct(bot), maxSpread);
            detail = who.empty() ? "Waiting for the party to recover." : (who + ".");
        }
        else
        {
            // No blocking condition — report what the advance action is up to,
            // using its per-tick phase token plus the route cache state.
            std::string const& phase = AI_VALUE(std::string&, "dungeon clear phase");
            uint32 const pathTarget = AI_VALUE(uint32, "dungeon clear long path target");
            bool const routeReady = next.has_value() && pathTarget == next->entry;

            if (phase == "recovering")
            {
                stateStr = "recovering";
                detail = "Stuck; replanning the route to " + bossName + ".";
            }
            else if (phase == "pursuing")
            {
                stateStr = "pursuing";
                detail = "Closing in on " + bossName + ".";
            }
            else if (next.has_value() && !routeReady)
            {
                // A boss is selected but no route to it is cached yet: the tank
                // is between picking the target and its first path build.
                stateStr = "pathing";
                detail = "Plotting a route to " + bossName + ".";
            }
            else if (phase == "moving" || bot->isMoving())
            {
                stateStr = "moving";
                detail = next.has_value() ? ("En route to " + bossName + ".") : "Advancing.";
            }
            else
            {
                stateStr = "idle";
                detail = next.has_value() ? ("Holding near " + bossName + ".") : "Idle.";
            }
        }
    }

    std::ostringstream addonMsg;
    addonMsg << "STATUS\t"
             << (enabled ? "1" : "0") << "\t"
             << (next.has_value() ? std::to_string(next->entry) : "0") << "\t"
             << (next.has_value() ? next->name : "None") << "\t"
             << (stall.empty() ? "" : stall) << "\t"
             << skipped.size() << "\t"
             << stateStr << "\t"
             << detail << "\t"
             // Trailing field (index 8): advanced-pull toggle, for the addon's
             // Pull button + readout. Appended so older addons ignoring it stay
             // compatible (same pattern as the BOSS wing field).
             << (pullMode ? "1" : "0");

    return addonMsg.str();
}

void DungeonClearUtil::PushStatus(PlayerbotAI* botAI)
{
    if (!botAI)
        return;

    std::string payload = BuildStatusPayload(botAI);
    SendAddonMessage(botAI, payload);

    // Keep the change-detector's STATUS snapshot in lock-step with an explicit
    // push so it doesn't re-emit the identical payload on the very next pass.
    // Deliberately leave lastBossSig untouched: a fresh `dc on` resets it to the
    // 0 sentinel via MarkActiveTank, so the first detector pass emits the boss
    // list exactly once; later skip/go changes flip the signature and re-push.
    if (Player* bot = botAI->GetBot())
    {
        std::lock_guard<std::mutex> lock(g_dcActiveTanksMutex);
        auto it = g_dcActiveTanks.find(bot->GetGUID());
        if (it != g_dcActiveTanks.end())
        {
            it->second.lastStatus = std::move(payload);
            it->second.primed = true;
        }
    }
}

void DungeonClearUtil::MarkActiveTank(ObjectGuid tank)
{
    std::lock_guard<std::mutex> lock(g_dcActiveTanksMutex);
    // Default-constructed state (primed=false) forces an emit on the first
    // detector pass even if nothing has changed yet.
    g_dcActiveTanks[tank];
}

void DungeonClearUtil::UnmarkActiveTank(ObjectGuid tank)
{
    std::lock_guard<std::mutex> lock(g_dcActiveTanksMutex);
    g_dcActiveTanks.erase(tank);
}

void DungeonClearUtil::TickStatusPushes(uint32 diff)
{
    // Throttle: detect at most every DC_PUSH_INTERVAL_MS. Status transitions are
    // human-perceptible at this granularity, and it keeps the per-tick cost off
    // the world loop.
    g_dcPushAccumMs += diff;
    if (g_dcPushAccumMs < DC_PUSH_INTERVAL_MS)
        return;
    g_dcPushAccumMs = 0;

    // Snapshot the GUIDs under lock, then do the heavier work (value reads,
    // packet builds) without holding it — SendAddonMessage / DoSpecificAction
    // must not run under our mutex.
    std::vector<ObjectGuid> tanks;
    {
        std::lock_guard<std::mutex> lock(g_dcActiveTanksMutex);
        if (g_dcActiveTanks.empty())
            return;
        tanks.reserve(g_dcActiveTanks.size());
        for (auto const& kv : g_dcActiveTanks)
            tanks.push_back(kv.first);
    }

    for (ObjectGuid guid : tanks)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld())
        {
            // Tank vanished without a clean dc off (logout, instance reset).
            // Drop it; the addon will resync on its next explicit request.
            std::lock_guard<std::mutex> lock(g_dcActiveTanksMutex);
            g_dcActiveTanks.erase(guid);
            continue;
        }

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            continue;

        std::string const payload = BuildStatusPayload(botAI);
        uint64 const bossSig = BuildBossSignature(botAI);

        bool emitStatus = false;
        bool emitBosses = false;
        {
            std::lock_guard<std::mutex> lock(g_dcActiveTanksMutex);
            auto it = g_dcActiveTanks.find(guid);
            if (it == g_dcActiveTanks.end())
                continue;  // unmarked between snapshot and now

            DcPushState& st = it->second;
            if (!st.primed || st.lastStatus != payload)
            {
                st.lastStatus = payload;
                emitStatus = true;
            }
            if (!st.primed || st.lastBossSig != bossSig)
            {
                st.lastBossSig = bossSig;
                emitBosses = true;
            }
            st.primed = true;
        }

        if (emitStatus)
            SendAddonMessage(botAI, payload);
        if (emitBosses)
            // Reuse the existing boss-list action (silent) so the BOSS_START /
            // BOSS* / BOSS_END framing and per-boss status logic live in one
            // place. "addon" param suppresses the chat echo.
            botAI->DoSpecificAction("dc bosses", Event("dc push", "addon"), true);
    }
}

