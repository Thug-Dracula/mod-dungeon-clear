/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCROUTEFILTER_H
#define _PLAYERBOT_DCROUTEFILTER_H

#include "Define.h"
#include "DetourExtended.h"   // dtQueryFilterExt

// Route-cost filter for the dungeon-clear long-range pathfinder. Extends the
// engine's dtQueryFilterExt with the DcNavPenaltyRegistry no-go volumes: a hand-
// authored axis-aligned box over a known-bad spot (e.g. the LBRS chasm climb)
// multiplies the cost of any A* edge through it so heavily that the detour always
// wins — steering the corridor off a navmesh shortcut a real player can't follow.
//
// It is a COST, never a hard rejection (passFilter is untouched), so a boxed edge
// stays traversable: if it is genuinely the only way through, the route still
// takes it and the bot is never stranded. Outside a volume getCost is byte-for-
// byte the stock dtQueryFilterExt cost (the base call below), so on every map
// without a volume — i.e. almost all of them — routing is completely unchanged.
//
// (A general steep-slope penalty was prototyped here and removed: Detour measures
// slope portal-midpoint to portal-midpoint, not along the walkable surface, so it
// misfired on legitimate staircases; and the mmap generator's own walkable limit
// is 60°, which overlaps the ~50° shortcut slope, so slope can't cleanly separate
// "bad shortcut" from "legit steep ramp". The surgical box is the reliable lever.)
//
// One instance per Build (cheap: one registry membership check in the ctor).
// getAreaCost / include/exclude flags are inherited unchanged, so callers still
// apply the liquid-avoidance area costs (DungeonClearGeometry::ApplyLiquidAreaCosts)
// on top.
class DcRouteFilter : public dtQueryFilterExt
{
public:
    explicit DcRouteFilter(uint32 mapId);

    float getCost(float const* pa, float const* pb,
        dtPolyRef prevRef, dtMeshTile const* prevTile, dtPoly const* prevPoly,
        dtPolyRef curRef, dtMeshTile const* curTile, dtPoly const* curPoly,
        dtPolyRef nextRef, dtMeshTile const* nextTile, dtPoly const* nextPoly) const override;

private:
    uint32 _mapId;
    bool   _hasVolumes;   // map has a no-go volume (per-edge box-test gate)
};

#endif
