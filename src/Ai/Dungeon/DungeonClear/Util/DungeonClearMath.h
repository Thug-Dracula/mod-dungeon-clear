/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARMATH_H
#define _PLAYERBOT_DUNGEONCLEARMATH_H

#include <cstdint>
#include <vector>

namespace DungeonClearMath
{
    // One forward hostile for the Dynamic-pull classifier. `chainEligible` is
    // pre-resolved by the caller from game state: true only when this mob is in
    // line of sight of the pull target and not separated from it by a closed door
    // (the far-targets scan ignores LOS, so this gate keeps a pack through a wall /
    // a floor away / behind a door from counting as a chaining neighbour). It is
    // ignored for mobs that turn out to share the target's own pack.
    struct DynPullMob
    {
        float x;
        float y;
        bool  chainEligible;
    };

    // Pure Dynamic-pull decision: should the pull on `mobs[targetIdx]` use the
    // careful Advanced pull-to-camp (return true) or a Leeroy (return false)?
    //   - Groups mobs into packs via connected components at `packRadius` (2D).
    //   - A single ISOLATED pack larger than `largePackThreshold` => Advanced.
    //   - Otherwise Advanced iff some OTHER pack has a `chainEligible` mob within
    //     `chainRadius` of a target-pack mob; else Leeroy.
    // Separated from the game-state resolution in DungeonClearUtil::Classify-
    // PullAdvanced so the clustering/threshold logic is unit-testable.
    bool ClassifyDynamicPull(std::vector<DynPullMob> const& mobs, std::size_t targetIdx,
                             float packRadius, float chainRadius,
                             std::uint32_t largePackThreshold);

    // Squared 2D distance from point P to segment (A,B).
    float DistSqToSegment2D(float px, float py,
                            float ax, float ay,
                            float bx, float by);

    // True if the 2D segment (A,B) intersects the axis-aligned box
    // [minX,maxX] x [minY,maxY]. Liang-Barsky slab clip: returns true even
    // when BOTH endpoints lie outside the box but the segment passes through
    // it (the case that matters for a thin door panel a path step straddles).
    bool SegmentIntersectsAABB2D(float ax, float ay, float bx, float by,
                                 float minX, float minY,
                                 float maxX, float maxY);
}

#endif
