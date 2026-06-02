/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearBlockingDoorValue.h"

#include <cmath>
#include <limits>

#include "GameObject.h"
#include "Map.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Playerbots.h"

namespace
{
    // How far ahead along the corridor to look for closed doors. Beyond
    // this distance we don't care — the chunked pathfinder will rebuild
    // by the time the bot gets there and the value will re-evaluate.
    constexpr float DOOR_LOOK_AHEAD = 80.0f;
    constexpr float DOOR_CORRIDOR_WIDTH = 5.0f;   // 2D radius from segment centerline

    float DistSqToSegment2D(float px, float py, float ax, float ay, float bx, float by)
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
    float accumulated = 0.0f;

    struct Seg { float ax, ay, bx, by; };
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
            segments.push_back(Seg{prevX, prevY, pathSeg.ex, pathSeg.ey});
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = pathSeg.ex;
            prevY = pathSeg.ey;
            if (accumulated >= DOOR_LOOK_AHEAD)
                break;
            continue;
        }

        for (G3D::Vector3 const& pt : pathSeg.polyline)
        {
            float const dx = pt.x - prevX;
            float const dy = pt.y - prevY;
            segments.push_back(Seg{prevX, prevY, pt.x, pt.y});
            accumulated += std::sqrt(dx * dx + dy * dy);
            prevX = pt.x;
            prevY = pt.y;
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

    float const widthSq = DOOR_CORRIDOR_WIDTH * DOOR_CORRIDOR_WIDTH;
    ObjectGuid best;
    float bestDistFromBotSq = std::numeric_limits<float>::max();
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    for (auto const& kv : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = kv.second;
        if (!go || !go->IsInWorld())
            continue;
        GameObjectTemplate const* info = go->GetGOInfo();
        if (!info || info->type != GAMEOBJECT_TYPE_DOOR)
            continue;
        // Doors flagged ignoredByPathing are authored as non-blocking for
        // navigation (decorative / always-passable) — never stall on them.
        if (info->door.ignoredByPathing)
            continue;
        // Is the door VISUALLY shut (the "block the corridor" state)? The
        // GOState→open/closed mapping is inverted by the door.startOpen
        // template flag: for a normal door (startOpen=0) GO_STATE_READY is
        // closed and GO_STATE_ACTIVE is open, but a gate that spawns open
        // (startOpen=1) sits at GO_STATE_READY while appearing OPEN. So a
        // raw "state == GO_STATE_READY" test false-positives on every
        // spawn-open gate. Mirror the client: closed iff READY xor startOpen.
        bool const startOpen = info->door.startOpen != 0;
        bool const closed = (go->GetGoState() == GO_STATE_READY) != startOpen;
        if (!closed)
            continue;

        float const gx = go->GetPositionX();
        float const gy = go->GetPositionY();
        float const distFromBotSq = (gx - botX) * (gx - botX) + (gy - botY) * (gy - botY);
        // Cull early — a door 200yd from the bot can't be in the first 80yd
        // of corridor either.
        if (distFromBotSq > (DOOR_LOOK_AHEAD + DOOR_CORRIDOR_WIDTH) *
                            (DOOR_LOOK_AHEAD + DOOR_CORRIDOR_WIDTH))
            continue;

        float minDistSq = std::numeric_limits<float>::max();
        for (auto const& seg : segments)
        {
            float const d2 = DistSqToSegment2D(gx, gy, seg.ax, seg.ay, seg.bx, seg.by);
            if (d2 < minDistSq)
                minDistSq = d2;
        }
        if (minDistSq > widthSq)
            continue;

        if (distFromBotSq < bestDistFromBotSq)
        {
            bestDistFromBotSq = distFromBotSq;
            best = go->GetGUID();
        }
    }
    return best;
}
