/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcEngageGeometry.h"

#include "DungeonClearUtil.h"   // DungeonClearUtil::GetLiveBoss (until DcTargeting moves)
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
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"

float DcEngageGeometry::AggroRangeOf(Player* bot, Unit* u, float fallback,
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
float DcEngageGeometry::BossEngageRange(Player* bot, AiObjectContext* ctx,
                                        DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return staticRange;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return staticRange;

    Creature* live = DungeonClearUtil::GetLiveBoss(bot, ctx, boss.entry);
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
float DcEngageGeometry::PullCommitRange(Player* bot, Unit* target, float staticRange)
{
    if (!bot || !target)
        return staticRange;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return staticRange;

    Creature* c = target->ToCreature();
    if (!c)
        return staticRange;             // players/totems: keep the fixed fallback

    float const margin = DcSettings::GetFloat(bot, "AggroRangeMargin");
    float const floorYd = DcSettings::GetFloat(bot, "PullCommitRangeFloor");
    float const capYd = DcSettings::GetFloat(bot, "PullCommitRangeCap");

    // Stop just as the tank would enter the pack's real aggro bubble: the core's own
    // notice distance (Creature::GetAggroRange, already level/config-scaled and
    // clamped 5-45yd) + both reaches + a small margin, so the commit fires a hair
    // BEFORE the pack would pull on its own. Identical formula to BossEngageRange.
    float range = c->GetAggroRange(bot) + c->GetCombatReach()
                + bot->GetCombatReach() + margin;
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}
bool DcEngageGeometry::IsAtBossEngage(Player* bot, AiObjectContext* ctx,
                                      DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return false;

    Creature* live = DungeonClearUtil::GetLiveBoss(bot, ctx, boss.entry);
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
bool DcEngageGeometry::IsDoorClosed(GameObject const* go)
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
bool DcEngageGeometry::ClosedDoorBetween(WorldObject* from, float tx, float ty,
                                         float tz, float corridorWidth)
{
    if (!from)
        return false;
    Map* map = from->GetMap();
    if (!map)
        return false;

    float const bx = from->GetPositionX();
    float const by = from->GetPositionY();
    float const bz = from->GetPositionZ();
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
float DcEngageGeometry::DistAlongPathToClosedDoor(
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
bool DcEngageGeometry::IsReachable(Player* bot, float x, float y, float z)
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
bool DcEngageGeometry::IsLevelReachable(Player* bot, Unit* u)
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
bool DcEngageGeometry::ComputeCorridor(Player* bot,
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
