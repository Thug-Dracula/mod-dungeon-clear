/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "CorridorCenter.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "Config.h"
#include "DetourCommon.h"
#include "DetourExtended.h"   // dtQueryFilterExt
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "PathGenerator.h"    // NAV_* flags, VERTEX_SIZE, INVALID_POLYREF
#include "Player.h"

namespace
{
    // findNearestPoly search box, matching the producers' own snap extents.
    float const POLY_EXTENTS[VERTEX_SIZE] = { 3.0f, 5.0f, 3.0f };

    // A vertical step larger than this between adjacent polyline points marks a
    // stair seam / off-mesh jump / ledge drop — a transition we must not nudge
    // sideways (it would shear the link off its endpoints). Horizontal hugging
    // along a ledge has no Z delta, so it is still centered normally.
    constexpr float JUMP_DZ = 1.8f;

    // Modest A* pool for the convenience overload's own query. findDistanceTo
    // Wall / findNearestPoly are local searches — they do not need the
    // dungeon-length pool the primary producer allocates.
    constexpr int CC_NODE_POOL = 2048;

    using ManagedQuery = std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)>;

    // Distance to the nearest wall from a G3D world point, plus the unit normal
    // pointing AWAY from that wall (in Detour {y,z,x} order). Returns false when
    // the point isn't on a poly or the query fails. On success `outDist` is the
    // wall distance and outNormal carries the (Detour-space) escape direction.
    bool WallProbe(dtNavMeshQuery const* query, dtQueryFilter const* filter,
                   G3D::Vector3 const& p, float searchRadius,
                   float& outDist, float outNormal[VERTEX_SIZE])
    {
        float const pos[VERTEX_SIZE] = { p.y, p.z, p.x };  // Detour order
        dtPolyRef ref = INVALID_POLYREF;
        float nearest[VERTEX_SIZE] = { 0, 0, 0 };
        if (dtStatusFailed(query->findNearestPoly(pos, POLY_EXTENTS, filter, &ref, nearest)) ||
            ref == INVALID_POLYREF)
            return false;

        float hitPos[VERTEX_SIZE];
        if (dtStatusFailed(query->findDistanceToWall(ref, pos, searchRadius, filter,
                                                     &outDist, hitPos, outNormal)))
            return false;
        return true;
    }

    // True iff a G3D world point still sits on a walkable poly.
    bool OnMesh(dtNavMeshQuery const* query, dtQueryFilter const* filter, G3D::Vector3 const& p)
    {
        float const pos[VERTEX_SIZE] = { p.y, p.z, p.x };
        dtPolyRef ref = INVALID_POLYREF;
        float nearest[VERTEX_SIZE] = { 0, 0, 0 };
        return dtStatusSucceed(query->findNearestPoly(pos, POLY_EXTENTS, filter, &ref, nearest)) &&
               ref != INVALID_POLYREF;
    }
}

CorridorCenter::Params CorridorCenter::LoadParams()
{
    Params p;
    p.enable    = sConfigMgr->GetOption<bool>("DungeonClear.PathCenterEnable", true);
    p.clearance = sConfigMgr->GetOption<float>("DungeonClear.PathWallClearance", 2.5f);
    p.maxPush   = sConfigMgr->GetOption<float>("DungeonClear.PathCenterMaxPush", 3.0f);
    p.clearance = std::max(0.0f, p.clearance);
    p.maxPush   = std::max(0.0f, p.maxPush);
    return p;
}

void CorridorCenter::Center(dtNavMeshQuery const* query, dtQueryFilter const* filter,
                            Player* bot, std::vector<G3D::Vector3>& pts, Params const& params)
{
    if (!params.enable || !query || !filter || !bot || pts.size() < 3 || params.clearance <= 0.0f)
        return;

    // Wide enough to see the OPPOSITE wall of a corridor up to ~2*clearance
    // wide, so the midpoint correction below can converge to the medial axis.
    float const searchRadius = params.clearance * 2.0f + 2.0f;

    // Never move the start (index 0) or the target (last point).
    for (size_t i = 1; i + 1 < pts.size(); ++i)
    {
        // Skip vertical transitions (jumps / stairs / ledge drops).
        if (std::fabs(pts[i].z - pts[i - 1].z) > JUMP_DZ ||
            std::fabs(pts[i + 1].z - pts[i].z) > JUMP_DZ)
            continue;

        float dist1 = 0.0f;
        float normal[VERTEX_SIZE] = { 0, 0, 0 };
        if (!WallProbe(query, filter, pts[i], searchRadius, dist1, normal))
            continue;
        if (dist1 >= params.clearance)
            continue;  // already clear of every wall — open room or wide hall

        // Escape direction back in G3D world order (normal is Detour {y,z,x}).
        G3D::Vector3 dir(normal[2], normal[0], normal[1]);
        float const dlen = dir.length();
        if (dlen < 0.5f)            // degenerate normal — skip rather than guess
            continue;
        dir /= dlen;

        float push = std::min(params.clearance - dist1, params.maxPush);

        // One-sided nudge off the nearest wall.
        G3D::Vector3 cand = pts[i] + dir * push;

        // In a narrow corridor that push heads toward the far wall; measure it
        // and, if it's close, settle on the midpoint of the two walls instead.
        float dist2 = 0.0f;
        float n2[VERTEX_SIZE] = { 0, 0, 0 };
        if (WallProbe(query, filter, cand, searchRadius, dist2, n2) && dist2 < params.clearance)
        {
            // Walls are (dist1) behind and (push + dist2) ahead along dir; the
            // centered offset from pts[i] is half their separation.
            float centered = std::clamp((push + dist2 - dist1) * 0.5f, 0.0f, params.maxPush);
            cand = pts[i] + dir * centered;
        }

        // Re-pin to a walkable Z and re-validate on-mesh; halve the push twice
        // before giving up, so a slightly-too-far nudge degrades gracefully.
        G3D::Vector3 accepted = pts[i];
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            G3D::Vector3 test = cand;
            bot->UpdateAllowedPositionZ(test.x, test.y, test.z);
            if (OnMesh(query, filter, test))
            {
                accepted = test;
                break;
            }
            // Pull halfway back toward the original point and retry.
            cand = (cand + pts[i]) * 0.5f;
        }
        pts[i] = accepted;
    }
}

void CorridorCenter::Center(Player* bot, std::vector<G3D::Vector3>& pts, Params const& params)
{
    if (!params.enable || !bot || pts.size() < 3 || params.clearance <= 0.0f)
        return;

    Map* map = bot->GetMap();
    if (!map)
        return;
    dtNavMesh const* navMesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
    if (!navMesh)
        return;

    ManagedQuery query(dtAllocNavMeshQuery(), &dtFreeNavMeshQuery);
    if (!query || dtStatusFailed(query->init(navMesh, CC_NODE_POOL)))
        return;

    dtQueryFilterExt filter;
    filter.setIncludeFlags(static_cast<uint16>(NAV_GROUND | NAV_WATER | NAV_MAGMA));
    filter.setExcludeFlags(0);

    Center(query.get(), &filter, bot, pts, params);
}
