/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearBlockingDoorValue.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "DBCStores.h"
#include "DBCStructure.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Playerbots.h"

namespace
{
    // How far ahead along the corridor to look for closed doors. Beyond
    // this distance we don't care — the chunked pathfinder will rebuild
    // by the time the bot gets there and the value will re-evaluate.
    constexpr float DOOR_LOOK_AHEAD = 80.0f;
    // Lateral slack added around the door's real model footprint before testing
    // whether a path leg passes through it. Covers the bot's body radius plus
    // the gap between the smoothed route centerline and the line the bot
    // actually walks / navmesh snapping. Small on purpose: the footprint box
    // already spans the true doorway (wide gate or narrow door alike), so this
    // is only tolerance, not a "is the door roughly near the path" band.
    constexpr float DOOR_PATH_PAD = 2.0f;
    // Vertical tolerance on top of the door model's own Z extent. A door whose
    // footprint doesn't overlap the leg's height (plus this) sits on another
    // floor (a stacked deck, a ramp over/under us) and is not blocking, even
    // when its 2D footprint lands under the path. Mirrors DC_DOOR_Z_BAND.
    constexpr float DOOR_Z_BAND = 6.0f;

    // Transform a world point into the door's local (orientation-aligned) frame,
    // matching GameObject::IsInRange2d. The door's model bounding box
    // (GameObjectDisplayInfo min/max, scaled) is axis-aligned in THIS frame, so
    // once both leg endpoints are transformed we can do a plain segment-vs-AABB
    // test against the real, oriented doorway footprint.
    inline void ToDoorLocal(GameObject const* go, float wx, float wy,
                            float& lx, float& ly)
    {
        float const sinA = std::sin(go->GetOrientation());
        float const cosA = std::cos(go->GetOrientation());
        float const ddx = wx - go->GetPositionX();
        float const ddy = wy - go->GetPositionY();
        lx = sinA * ddx + cosA * ddy;
        ly = cosA * ddx - sinA * ddy;
    }

    // True when a path leg actually passes THROUGH this door's footprint, rather
    // than merely running near its origin. This is the whole point of the value:
    // doors a route runs PAST (e.g. Shadowfang Keep's first courtyard gate, which
    // you skirt to reach the prison event that unlocks it) must NOT be flagged —
    // only doors the corridor transits.
    bool PathLegCrossesDoor(GameObject const* go,
                            GameObjectDisplayInfoEntry const* info,
                            float ax, float ay, float az,
                            float bx, float by, float bz)
    {
        float const scale = go->GetObjectScale();
        float const gz = go->GetPositionZ();
        // Floor / height gate first (cheap): the leg's Z span must overlap the
        // door's vertical extent, padded.
        float const doorLoZ = gz + info->minZ * scale - DOOR_Z_BAND;
        float const doorHiZ = gz + info->maxZ * scale + DOOR_Z_BAND;
        float const legLoZ = std::min(az, bz);
        float const legHiZ = std::max(az, bz);
        if (legHiZ < doorLoZ || legLoZ > doorHiZ)
            return false;

        float lax, lay, lbx, lby;
        ToDoorLocal(go, ax, ay, lax, lay);
        ToDoorLocal(go, bx, by, lbx, lby);

        float const minX = info->minX * scale - DOOR_PATH_PAD;
        float const maxX = info->maxX * scale + DOOR_PATH_PAD;
        float const minY = info->minY * scale - DOOR_PATH_PAD;
        float const maxY = info->maxY * scale + DOOR_PATH_PAD;
        return DungeonClearMath::SegmentIntersectsAABB2D(lax, lay, lbx, lby,
                                                         minX, minY, maxX, maxY);
    }

}

ObjectGuid DungeonClearBlockingDoorValue::Calculate()
{
    if (!bot || !bot->IsInWorld())
        return ObjectGuid::Empty;
    if (!AI_VALUE(bool, "dungeon clear enabled") || AI_VALUE(bool, "dungeon clear paused"))
        return ObjectGuid::Empty;

    Map* map = bot->GetMap();
    if (!map)
        return ObjectGuid::Empty;

    ChunkedPathfinder::Result const& path =
        AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
    if (!path.reachable || path.segments.empty())
        return ObjectGuid::Empty;

    // Build a series of 2D segments tracing the actual walked route, anchored
    // on the bot's current position and clipped to DOOR_LOOK_AHEAD yards along
    // the polyline. Anything past the look-ahead doesn't count — by the time
    // the bot walks there, the value will re-evaluate.
    //
    // CRITICAL: chain the per-segment `polyline` points, NOT the segment
    // endpoints (`seg.ex/ey`). The primary producer (LongRangePathfinder)
    // emits the ENTIRE winding route as a single PathSegment whose ex/ey is
    // only the final endpoint (the boss) — so chaining endpoints collapses the
    // corridor to a straight bee-line from the bot to the boss. A bee-line
    // sweeps through rooms the real path never enters and flags any closed door
    // that happens to lie near that straight line, even one nowhere on the
    // route. The polyline is the real smoothed corridor geometry.
    float prevX = bot->GetPositionX();
    float prevY = bot->GetPositionY();
    float prevZ = bot->GetPositionZ();
    float accumulated = 0.0f;

    struct Seg { float ax, ay, az, bx, by, bz; };
    std::vector<Seg> segments;

    bool reachedLookAhead = false;
    for (PathSegment const& pathSeg : path.segments)
    {
        // Anchored segments collapse to a single polyline point; non-anchored
        // segments carry the full smoothed corridor. Fall back to the endpoint
        // only if a segment somehow has no polyline at all.
        if (pathSeg.polyline.empty())
        {
            float const dx = pathSeg.ex - prevX;
            float const dy = pathSeg.ey - prevY;
            segments.push_back(Seg{prevX, prevY, prevZ, pathSeg.ex, pathSeg.ey, pathSeg.ez});
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = pathSeg.ex;
            prevY = pathSeg.ey;
            prevZ = pathSeg.ez;
            if (accumulated >= DOOR_LOOK_AHEAD)
                break;
            continue;
        }

        for (G3D::Vector3 const& pt : pathSeg.polyline)
        {
            float const dx = pt.x - prevX;
            float const dy = pt.y - prevY;
            segments.push_back(Seg{prevX, prevY, prevZ, pt.x, pt.y, pt.z});
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = pt.x;
            prevY = pt.y;
            prevZ = pt.z;
            if (accumulated >= DOOR_LOOK_AHEAD)
            {
                reachedLookAhead = true;
                break;
            }
        }
        if (reachedLookAhead)
            break;
    }
    if (segments.empty())
        return ObjectGuid::Empty;

    // Generous cull radius: the door's footprint must be within the look-ahead
    // plus room for a large gate's half-extent. Anything past this can't be on
    // the first DOOR_LOOK_AHEAD yards of corridor.
    constexpr float CULL = DOOR_LOOK_AHEAD + 20.0f;
    ObjectGuid best;
    float bestDistFromBotSq = std::numeric_limits<float>::max();
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    for (auto const& kv : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = kv.second;
        // Blocking door currently shut (the "block the corridor" state). The
        // helper folds the type / ignoredByPathing / startOpen-inverted GOState
        // checks; see DcEngageGeometry::IsDoorClosed.
        if (!DcEngageGeometry::IsDoorClosed(go))
            continue;
        GameObjectTemplate const* tmpl = go->GetGOInfo();

        float const gx = go->GetPositionX();
        float const gy = go->GetPositionY();
        float const distFromBotSq = (gx - botX) * (gx - botX) + (gy - botY) * (gy - botY);
        if (distFromBotSq > CULL * CULL)
            continue;

        // Resolve the door's real model footprint. Without it we can't tell
        // "through" from "past"; fall back to nothing (don't flag) rather than
        // re-introduce the origin-proximity false positives this replaced.
        GameObjectDisplayInfoEntry const* disp =
            sGameObjectDisplayInfoStore.LookupEntry(tmpl->displayId);
        if (!disp)
        {
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] blocking-door: closed door {} (entry {}) has no display "
                      "footprint -> NOT flagged",
                      bot->GetName(), go->GetGUID().ToString(), tmpl->entry);
            continue;
        }

        // Flag ONLY when a route leg actually transits the doorway footprint.
        // A door the corridor merely runs past (its model off to the side of
        // every leg) is left alone, so the tank keeps moving toward the boss /
        // event that opens it instead of parking beside a door it never enters.
        bool crosses = false;
        for (auto const& seg : segments)
        {
            if (PathLegCrossesDoor(go, disp, seg.ax, seg.ay, seg.az,
                                   seg.bx, seg.by, seg.bz))
            {
                crosses = true;
                break;
            }
        }
        if (!crosses)
            continue;

        if (distFromBotSq < bestDistFromBotSq)
        {
            bestDistFromBotSq = distFromBotSq;
            best = go->GetGUID();
        }
    }

    if (!best.IsEmpty())
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] blocking-door: flagged {} as corridor-blocking",
                  bot->GetName(), best.ToString());
    return best;
}
