/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMath.h"

#include <cmath>
#include <cstddef>

std::uint32_t DungeonClearMath::EstimateAggroCount(std::vector<DynPullMob> const& mobs,
                                                   std::size_t targetIdx,
                                                   float combatSpread,
                                                   float assistRadius,
                                                   float zTolerance,
                                                   std::vector<std::size_t>* countedOut)
{
    std::size_t const n = mobs.size();
    if (n == 0 || targetIdx >= n)
        return 0;

    auto dist2d = [&](std::size_t a, std::size_t b)
    {
        float const dx = mobs[a].x - mobs[b].x;
        float const dy = mobs[a].y - mobs[b].y;
        return std::sqrt(dx * dx + dy * dy);
    };
    // Same floor: the inter-mob height gap is within tolerance. WotLK floor-to-
    // floor gaps comfortably exceed it, so this separates a ledge/ramp mob
    // overhead from one that shares the camp's floor.
    auto sameLevel = [&](std::size_t a, std::size_t b)
    {
        return std::fabs(mobs[a].z - mobs[b].z) <= zTolerance;
    };

    // Engine-pack identity is atomic: members sharing a non-zero packId pull as
    // one unit regardless of distance or height (a formation / linked respawn).
    // Union them so a partially-counted pack can be closed to its full size.
    // n is tiny, so O(n^2) union-find is fine.
    std::vector<std::size_t> parent(n);
    for (std::size_t i = 0; i < n; ++i)
        parent[i] = i;
    auto find = [&parent](std::size_t a)
    {
        while (parent[a] != a)
        {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    };
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j)
            if (mobs[i].packId != 0 && mobs[i].packId == mobs[j].packId)
                parent[find(i)] = find(j);

    std::vector<char> inSet(n, 0);

    // Seed: the camp spot is the target's position. A mob proximity-aggros when a
    // player enters ITS aggro radius, and in a Leeroy the whole party piles onto
    // the target, so a mob counts when it sits within its own reach (+ combat
    // drift) of the target, on the same level and reachable. The target's own
    // atomic pack pulls regardless of distance/height.
    std::size_t const targetRoot = find(targetIdx);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (find(i) == targetRoot)
        {
            inSet[i] = 1;  // target + its atomic pack
            continue;
        }
        if (mobs[i].chainEligible && sameLevel(i, targetIdx)
            && dist2d(i, targetIdx) <= mobs[i].aggroReach + combatSpread)
            inSet[i] = 1;
    }

    // One assist hop (no transitivity). A chainEligible mob within assistRadius of
    // a SEED mob joins via CallForHelp, but proximity aggro from a fixed camp does
    // not chain, so an assisted mob does not seed further proximity or assist.
    // Iterate over a SNAPSHOT of the seed so newly-added mobs can't propagate.
    std::vector<char> const seed = inSet;
    for (std::size_t j = 0; j < n; ++j)
    {
        if (seed[j] || !mobs[j].chainEligible)
            continue;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!seed[i])
                continue;
            if (sameLevel(i, j) && dist2d(i, j) <= assistRadius)
            {
                inSet[j] = 1;
                break;
            }
        }
    }

    // Formation closure: a counted member drags its whole pack in — you cannot
    // pull half a formation. This completes packs; it does not re-seed proximity
    // or assist, so it cannot cascade.
    for (std::size_t i = 0; i < n; ++i)
    {
        if (!inSet[i] || mobs[i].packId == 0)
            continue;
        std::size_t const r = find(i);
        for (std::size_t k = 0; k < n; ++k)
            if (find(k) == r)
                inSet[k] = 1;
    }

    if (countedOut)
        countedOut->clear();
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (inSet[i])
        {
            ++count;
            if (countedOut)
                countedOut->push_back(i);
        }
    return count;
}

bool DungeonClearMath::ShouldAbortPullForCc(bool impaired, std::uint32_t ccSince,
                                            std::uint32_t now, std::uint32_t graceMs,
                                            std::uint32_t& ccSinceOut)
{
    if (!impaired)
    {
        ccSinceOut = 0;
        return false;
    }
    // Arm the latch on the first impaired tick. Never store 0 (it means "clear"),
    // so a tank impaired at the very first millisecond still latches.
    std::uint32_t const start = ccSince != 0 ? ccSince : (now != 0 ? now : 1u);
    ccSinceOut = start;
    // Guard the unsigned subtraction: start is normally <= now (latched this tick
    // or earlier), but the now==0 corner can leave start (1) ahead of now. Clamp to
    // 0 elapsed there. graceMs == 0 => aborts immediately on the first tick.
    std::uint32_t const elapsed = now >= start ? now - start : 0u;
    return elapsed >= graceMs;
}

bool DungeonClearMath::ShouldDropPullVerdict(bool targetPresent, std::uint32_t lostSince,
                                             std::uint32_t now, std::uint32_t graceMs,
                                             std::uint32_t& lostSinceOut)
{
    if (targetPresent)
    {
        lostSinceOut = 0;
        return false;
    }
    // Arm the latch on the first lost tick. Never store 0 (it means "present"),
    // so a target lost at the very first millisecond still latches.
    std::uint32_t const start = lostSince != 0 ? lostSince : (now != 0 ? now : 1u);
    lostSinceOut = start;
    // Guard the unsigned subtraction: start is normally <= now (latched this tick
    // or earlier), but the now==0 corner can leave start (1) ahead of now. Clamp to
    // 0 elapsed there. graceMs == 0 => drops immediately on the first tick.
    std::uint32_t const elapsed = now >= start ? now - start : 0u;
    return elapsed >= graceMs;
}

bool DungeonClearMath::ShouldRollInForLeeroy(std::uint32_t decision, bool targetAlive,
                                             float tankToTarget2d, float commitRange,
                                             float lead)
{
    // Only a standing LEEROY verdict (1) commits the tank to charging the pack.
    // No verdict (0) means it is still sizing the pack up, and Advanced (2)
    // hands the party to hold-at-camp — neither may release the scout lag.
    if (decision != 1u || !targetAlive)
        return false;
    // The boundary is inclusive: at exactly commitRange + lead the tank is
    // committing, so the party starts rolling.
    return tankToTarget2d <= commitRange + lead;
}

float DungeonClearMath::DistSqToSegment2D(float px, float py,
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

bool DungeonClearMath::SegmentIntersectsAABB2D(float ax, float ay,
                                               float bx, float by,
                                               float minX, float minY,
                                               float maxX, float maxY)
{
    // Parametric segment P(t) = A + t*(B-A), t in [0,1]. Clip the interval
    // against each box slab; if anything survives, the segment touches the box.
    float const dx = bx - ax;
    float const dy = by - ay;
    float t0 = 0.0f;
    float t1 = 1.0f;

    // p = -direction component, q = distance to the slab edge. The pair
    // (-dx, ax-minX), (dx, maxX-ax), (-dy, ay-minY), (dy, maxY-ay) covers the
    // four edges (left, right, bottom, top).
    float const p[4] = {-dx, dx, -dy, dy};
    float const q[4] = {ax - minX, maxX - ax, ay - minY, maxY - ay};

    for (int i = 0; i < 4; ++i)
    {
        if (p[i] == 0.0f)
        {
            // Segment parallel to this slab: outside it -> no intersection.
            if (q[i] < 0.0f)
                return false;
            continue;
        }
        float const r = q[i] / p[i];
        if (p[i] < 0.0f)
        {
            if (r > t1)
                return false;
            if (r > t0)
                t0 = r;
        }
        else
        {
            if (r < t0)
                return false;
            if (r < t1)
                t1 = r;
        }
    }
    return t0 <= t1;
}
