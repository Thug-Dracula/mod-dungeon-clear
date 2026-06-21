/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcNavPenaltyRegistry.h"

#include <array>

namespace
{
    // ---- the table ------------------------------------------------------
    // One row per navmesh shortcut a real player can't follow. Each box spans
    // only the MIDDLE Z band of its climb — the legitimate floor below and ledge
    // platform above sit just outside it, so a route that genuinely belongs down
    // there or up there is untaxed; only an edge climbing the face pays. A stiff
    // multiplier makes the A* corridor take the real way around whenever one
    // exists; it stays a cost, so the spot is never made unreachable.
    //
    // Lower Blackrock Spire (map 229), #1 — the big chasm climb. The navmesh
    // stitches a walkable poly up the wall between the lower walkway (~z30) and an
    // upper ledge (~z58), so the tank climbs a near-vertical face the party can't
    // follow. Observed bot traversal:
    //     [-127.33, -402.11, 30.32]  ->  [-124.88, -378.42, 58.40]
    // (≈28yd rise over ≈24yd ground = ~50°). Box mid-band z 33..56.
    //
    // Lower Blackrock Spire (map 229), #2 — a small ledge-hop further along the
    // (now-corrected) route, where some bots wedged on the step. Same class, much
    // smaller. Observed traversal:
    //     [-61.70, -382.77, 48.88]  <->  [-64.34, -378.49, 54.70]
    // (≈5.8yd rise over ≈5yd ground = ~49°). Tight box hugging the two endpoints,
    // mid-band z 49.4..54.2 so the lower walkway (≤~49) and the upper platform
    // (≥~54.7, which the proper route reaches from another direction) stay untaxed.
    constexpr std::array<DcNavPenaltyVolume, 2> kVolumes = {{
        { 229, -134.0f, -406.0f, 33.0f, -118.0f, -374.0f, 56.0f, 40.0f },
        { 229,  -65.5f, -384.0f, 49.4f,  -60.5f, -377.0f, 54.2f, 40.0f },
    }};
}

bool DcNavPenaltyRegistry::HasVolumes(uint32 mapId)
{
    for (auto const& v : kVolumes)
        if (v.mapId == mapId)
            return true;
    return false;
}

float DcNavPenaltyRegistry::PenaltyAt(uint32 mapId, float x, float y, float z)
{
    float worst = 1.0f;
    for (auto const& v : kVolumes)
    {
        if (v.mapId != mapId)
            continue;
        if (x < v.minX || x > v.maxX)
            continue;
        if (y < v.minY || y > v.maxY)
            continue;
        if (z < v.minZ || z > v.maxZ)
            continue;
        if (v.costMult > worst)
            worst = v.costMult;
    }
    return worst;
}
