/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNavPenaltyRegistry.h"

// Pure tests for the hand-authored no-go volume table. No navmesh / map data
// required, so these run in every build (unlike the Tier-2 nav geometry suite).

TEST(DcNavPenaltyRegistry, ReportsMapsWithVolumes)
{
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(229));   // Lower Blackrock Spire
    EXPECT_FALSE(DcNavPenaltyRegistry::HasVolumes(0));     // no rows
    EXPECT_FALSE(DcNavPenaltyRegistry::HasVolumes(230));   // BRD — no rows
}

TEST(DcNavPenaltyRegistry, PenalizesInsideTheLbrsShaft)
{
    // The midpoint of the observed shortcut climb
    //   [-127.33,-402.11,30.32] -> [-124.88,-378.42,58.40]
    // is ≈(-126.1,-390.3,44.4): squarely inside the box, so it must be taxed.
    float const p = DcNavPenaltyRegistry::PenaltyAt(229, -126.1f, -390.3f, 44.4f);
    EXPECT_GT(p, 1.0f);
}

TEST(DcNavPenaltyRegistry, PenalizesInsideTheLbrsLedgeHop)
{
    // The midpoint of the second (small) shortcut
    //   [-61.70,-382.77,48.88] <-> [-64.34,-378.49,54.70]
    // is ≈(-63.0,-380.6,51.8): inside box #2's mid-Z band, so it is taxed.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(229, -63.0f, -380.6f, 51.8f), 1.0f);
    // The lower walkway end (z below the band) is the legit approach — untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, -61.7f, -382.77f, 48.88f), 1.0f);
    // The upper platform end (z above the band), reached by the proper route from
    // another direction — untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, -64.34f, -378.49f, 54.7f), 1.0f);
}

TEST(DcNavPenaltyRegistry, DoesNotPenalizeOutsideTheBox)
{
    // Same X/Y as the shaft but down on the lower floor (below the mid-Z band):
    // a route that legitimately belongs at the bottom must not be taxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, -126.0f, -390.0f, 30.0f), 1.0f);
    // Far away on the same map.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, 200.0f, 200.0f, 44.0f), 1.0f);
    // Inside the box geometrically, but a different map → no volume applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, -126.1f, -390.3f, 44.4f), 1.0f);
}
