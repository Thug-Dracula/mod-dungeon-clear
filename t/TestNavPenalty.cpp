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
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(556));   // Sethekk Halls
    EXPECT_FALSE(DcNavPenaltyRegistry::HasVolumes(0));     // no rows
    EXPECT_FALSE(DcNavPenaltyRegistry::HasVolumes(230));   // BRD — no rows
}

TEST(DcNavPenaltyRegistry, PenalizesTheSethekkBackDoorRamp)
{
    // A point partway up the narrow x≈45 shortcut ramp (door y151,z0 ->
    // platform y250,z27): squarely inside the box, so it must be taxed.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(556, 45.0f, 200.0f, 14.0f), 1.0f);

    // The legitimate western approach arrives on the platform at y>=250 — north
    // of the box. Ikiss's own position must be untaxed so the final hop is free.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, 44.7f, 287.0f, 25.2f), 1.0f);

    // The lower lobby south of the door (y<150) is the normal pre-Syth floor —
    // untaxed so early routing is unchanged.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, 45.0f, 130.0f, 0.3f), 1.0f);

    // The west ramp the long way actually uses (~(-250, 210)) is far from the
    // box — untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, -250.0f, 210.0f, 27.0f), 1.0f);
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
