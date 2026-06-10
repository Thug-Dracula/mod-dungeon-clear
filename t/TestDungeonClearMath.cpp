/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "DungeonClearMath.h"

// Test point directly on the segment (midpoint)
TEST(DungeonClearMathTest, PointOnSegmentMidpoint)
{
    float px = 5.0f;
    float py = 0.0f;
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // Midpoint (5,0) on segment (0,0)-(10,0) -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(px, py, ax, ay, bx, by), 0.0f, 1e-5f);
}

// Test point on the segment endpoints
TEST(DungeonClearMathTest, PointOnSegmentEndpoints)
{
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // At A (0,0) -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(0.0f, 0.0f, ax, ay, bx, by), 0.0f, 1e-5f);

    // At B (10,0) -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(10.0f, 0.0f, ax, ay, bx, by), 0.0f, 1e-5f);
}

// Test point collinear with segment but outside the endpoints
TEST(DungeonClearMathTest, PointCollinearOutsideSegment)
{
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // Before A (-2,0) -> closest point should be A (0,0) -> distance squared should be 4
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(-2.0f, 0.0f, ax, ay, bx, by), 4.0f, 1e-5f);

    // After B (12,0) -> closest point should be B (10,0) -> distance squared should be 4
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(12.0f, 0.0f, ax, ay, bx, by), 4.0f, 1e-5f);
}

// Test point with perpendicular offset from segment
TEST(DungeonClearMathTest, PointWithPerpendicularOffset)
{
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // Above midpoint (5,3) -> closest point should be (5,0) -> distance squared should be 9
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(5.0f, 3.0f, ax, ay, bx, by), 9.0f, 1e-5f);

    // Below midpoint (5,-4) -> closest point should be (5,0) -> distance squared should be 16
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(5.0f, -4.0f, ax, ay, bx, by), 16.0f, 1e-5f);
}

// Test zero-length segment (A == B)
TEST(DungeonClearMathTest, ZeroLengthSegment)
{
    float ax = 5.0f;
    float ay = 5.0f;
    float bx = 5.0f;
    float by = 5.0f;

    // Segment is a single point (5,5). Distance to (5,8) should be 3 -> distance squared should be 9
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(5.0f, 8.0f, ax, ay, bx, by), 9.0f, 1e-5f);

    // Distance to (2,5) should be 3 -> distance squared should be 9
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(2.0f, 5.0f, ax, ay, bx, by), 9.0f, 1e-5f);
}

// Test diagonal segment
TEST(DungeonClearMathTest, DiagonalSegment)
{
    // Segment from (0,0) to (6,6)
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 6.0f;
    float by = 6.0f;

    // Point (0,6) -> projection on line is (3,3) -> distance to (3,3) is sqrt(3^2 + 3^2) = sqrt(18)
    // Distance squared should be 18
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(0.0f, 6.0f, ax, ay, bx, by), 18.0f, 1e-5f);

    // Point (3,3) is on the diagonal -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(3.0f, 3.0f, ax, ay, bx, by), 0.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// Dynamic pull aggro estimate (Leeroy vs Advanced). combatSpread 6 and zTolerance
// 5 mirror the shipped defaults; assistRadius 10 is the engine's default
// CreatureFamilyAssistanceRadius (the caller passes the live config value). The
// caller's Leeroy ceiling is 5 (Advanced when count > 5). Each mob's aggroReach is
// the 6th brace field; the common trash radius used below is ~18yd (a mid-level
// mob's real aggro radius).
// ---------------------------------------------------------------------------
namespace
{
    using DungeonClearMath::DynPullMob;
    constexpr float kSpread = 6.0f;
    constexpr float kAssistR = 10.0f;  // engine CreatureFamilyAssistanceRadius default
    constexpr float kZTol = 5.0f;   // DC_Z_LEVEL_TOLERANCE
    constexpr unsigned kCeil = 5u;  // PullDynamicMaxLeeroyMobs
    constexpr float kReach = 18.0f; // a representative mob aggro radius

    unsigned Count(std::vector<DynPullMob> const& m, std::size_t t)
    {
        return DungeonClearMath::EstimateAggroCount(m, t, kSpread, kAssistR, kZTol);
    }
    // Advanced iff the estimate exceeds the Leeroy ceiling (mirrors the caller).
    bool Advanced(std::vector<DynPullMob> const& m, std::size_t t)
    {
        return Count(m, t) > kCeil;
    }
}

// A single lone mob -> count 1 -> Leeroy.
TEST(DungeonClearDynamicPullTest, SingleMobLeeroy)
{
    std::vector<DynPullMob> mobs = { {0.0f, 0.0f, 0.0f, false, 0u, kReach} };
    EXPECT_EQ(Count(mobs, 0), 1u);
    EXPECT_FALSE(Advanced(mobs, 0));
}

// One bunched pack of 3 (all within aggro reach of the camp) -> count 3 -> Leeroy.
TEST(DungeonClearDynamicPullTest, LoneSmallPackLeeroy)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {3.0f, 0.0f, 0.0f, true, 0u, kReach},
        {0.0f, 4.0f, 0.0f, true, 0u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 3u);
    EXPECT_FALSE(Advanced(mobs, 0));
}

// The motivating case: THREE loose 2-mob packs near each other, all within aggro
// reach of the camp spot -> 6 mobs aggro -> count 6 > ceiling 5 -> Advanced. The
// old pack-adjacency model also Advanced here (any neighbour flips it), but now it
// is because SIX bodies pile in, and raising the ceiling to 6 would Leeroy them.
TEST(DungeonClearDynamicPullTest, ThreeClusteredPairsAdvanced)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {3.0f, 0.0f, 0.0f, true, 0u, kReach},
        {10.0f, 0.0f, 0.0f, true, 0u, kReach}, {13.0f, 0.0f, 0.0f, true, 0u, kReach},
        {0.0f, 10.0f, 0.0f, true, 0u, kReach}, {3.0f, 10.0f, 0.0f, true, 0u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 6u);
    EXPECT_TRUE(Advanced(mobs, 0));
}

// The SAME six mobs, but the two other pairs are spaced well beyond aggro reach of
// the camp (and of each other) -> only the target pair aggros -> count 2 ->
// Leeroy. This is the fix: spaced trivial packs are no longer all camp-pulled.
TEST(DungeonClearDynamicPullTest, ThreeSpacedPairsLeeroy)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {3.0f, 0.0f, 0.0f, true, 0u, kReach},
        // 40yd / 80yd out: beyond reach(18)+spread(6)=24 of the camp, and beyond an
        // assist hop from the target pair.
        {40.0f, 0.0f, 0.0f, true, 0u, kReach}, {43.0f, 0.0f, 0.0f, true, 0u, kReach},
        {80.0f, 0.0f, 0.0f, true, 0u, kReach}, {83.0f, 0.0f, 0.0f, true, 0u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 2u);
    EXPECT_FALSE(Advanced(mobs, 0));
}

// A mob within its aggro reach + combat spread of the camp counts even though it
// is outside dead-centre reach: reach 18 + spread 6 = 24, mob at 22yd -> counts.
// The "messy combat drift" case.
TEST(DungeonClearDynamicPullTest, CombatSpreadPullsInDriftMob)
{
    std::vector<DynPullMob> single = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {22.0f, 0.0f, 0.0f, true, 0u, kReach}
    };
    EXPECT_EQ(Count(single, 0), 2u);  // 22 <= 18 + 6

    // Push it to 25yd (> 24) and, with no assist bridge, it drops out -> count 1.
    std::vector<DynPullMob> beyond = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {25.0f, 0.0f, 0.0f, true, 0u, kReach}
    };
    EXPECT_EQ(Count(beyond, 0), 1u);
}

// A mob within aggro reach but NOT chainEligible (behind a wall / door / a floor
// away) does not aggro -> not counted.
TEST(DungeonClearDynamicPullTest, NotEligibleNotCounted)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {5.0f, 0.0f, 0.0f, false, 0u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 1u);  // packmate gated out
}

// One assist hop, no transitivity. A is the target; B aggros from proximity; C is
// within assistRadius of B (a seed mob) -> joins; D is within assistRadius of C
// only (NOT of any seed) -> must NOT join, because assisted mobs don't chain.
TEST(DungeonClearDynamicPullTest, AssistHopIsExactlyOneRing)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach},   // A target
        {3.0f, 0.0f, 0.0f, true, 0u, kReach},    // B proximity-aggros (seed)
        {11.0f, 0.0f, 0.0f, true, 0u, 1.0f},     // C: 8yd from B (<=assist 10) -> hop
        {20.0f, 0.0f, 0.0f, true, 0u, 1.0f}      // D: 9yd from C, 17yd from B -> NO
    };
    // tiny reach on C/D means they cannot proximity-aggro the camp themselves.
    EXPECT_EQ(Count(mobs, 0), 3u);  // A + B + C, NOT D
}

// 3D: a mob within aggro reach in plan view but a full floor (20yd) ABOVE the
// target -> not counted. A flat 2D estimate would have pulled it in.
TEST(DungeonClearDynamicPullTest, MobOnFloorAboveNotCounted)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {5.0f, 0.0f, 20.0f, true, 0u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 1u);
}

// 3D: the SAME 2D geometry but on our level (within zTolerance) -> counted,
// proving the height gate is what excludes the overhead mob, not the layout.
TEST(DungeonClearDynamicPullTest, MobOnSameLevelCounted)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach}, {5.0f, 0.0f, 3.0f, true, 0u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 2u);
}

// Formation closure: a strung-out FORMATION (shared packId) where only the target
// is near the camp still counts in FULL — you can't pull half a formation. Six
// members, each 20yd from the next (beyond proximity/assist of the others), all
// share id 7 -> count 6 > ceiling -> Advanced. With packId 0 they'd be lone mobs
// out of reach and the count would be 1.
TEST(DungeonClearDynamicPullTest, SpreadFormationCountsInFull)
{
    std::vector<DynPullMob> mobs = {
        {0.0f,  0.0f, 0.0f, false, 7u, kReach}, {20.0f, 0.0f, 0.0f, true, 7u, kReach},
        {40.0f, 0.0f, 0.0f, true,  7u, kReach}, {60.0f, 0.0f, 0.0f, true, 7u, kReach},
        {80.0f, 0.0f, 0.0f, true,  7u, kReach}, {100.0f, 0.0f, 0.0f, true, 7u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 6u);
    EXPECT_TRUE(Advanced(mobs, 0));
}

// Formation closure unions across HEIGHT too: a formation is atomic even if a
// member sits a floor up. Six members share id 3, one of them overhead (20yd up)
// -> still counted -> Advanced. Contrast MobOnFloorAboveNotCounted, where a
// packId-0 overhead mob is correctly excluded by the z-gate.
TEST(DungeonClearDynamicPullTest, FormationClosureUnionsAcrossFloors)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 3u, kReach}, {2.0f, 0.0f, 0.0f, true, 3u, kReach},
        {4.0f, 0.0f, 0.0f, true,  3u, kReach}, {0.0f, 2.0f, 0.0f, true, 3u, kReach},
        {2.0f, 2.0f, 0.0f, true,  3u, kReach},
        {2.0f, 1.0f, 20.0f, false, 3u, kReach}  // overhead but same formation
    };
    EXPECT_EQ(Count(mobs, 0), 6u);
    EXPECT_TRUE(Advanced(mobs, 0));
}

// DISTINCT packIds do NOT union. Target's own 3-mob formation (id 1) is near the
// camp; a separate 3-mob formation (id 2) is 50yd away, out of reach and not an
// assist hop -> only id 1 counts -> count 3 -> Leeroy.
TEST(DungeonClearDynamicPullTest, DistinctFormationsDoNotUnion)
{
    std::vector<DynPullMob> mobs = {
        {0.0f,  0.0f, 0.0f, false, 1u, kReach}, {4.0f,  0.0f, 0.0f, true, 1u, kReach},
        {8.0f,  0.0f, 0.0f, true,  1u, kReach},
        {0.0f, 50.0f, 0.0f, true,  2u, kReach}, {4.0f, 50.0f, 0.0f, true, 2u, kReach},
        {8.0f, 50.0f, 0.0f, true,  2u, kReach}
    };
    EXPECT_EQ(Count(mobs, 0), 3u);
    EXPECT_FALSE(Advanced(mobs, 0));
}

// ---------------------------------------------------------------------------
// ShouldAbortPullForCc — the pull CC-assist grace gate.
// ---------------------------------------------------------------------------
using DungeonClearMath::ShouldAbortPullForCc;

// Not impaired: never abort, latch cleared to 0 (even from a stale value).
TEST(DungeonClearCcAssistTest, NotImpairedClearsLatch)
{
    std::uint32_t out = 12345u;
    EXPECT_FALSE(ShouldAbortPullForCc(false, 10000u, 20000u, 1000u, out));
    EXPECT_EQ(out, 0u);
}

// First impaired tick arms the latch (to `now`) but does not yet abort.
TEST(DungeonClearCcAssistTest, FirstImpairedTickArmsButHolds)
{
    std::uint32_t out = 0u;
    EXPECT_FALSE(ShouldAbortPullForCc(true, 0u, 5000u, 1000u, out));
    EXPECT_EQ(out, 5000u);
}

// Sustained impairment aborts once the grace has elapsed; the latch is preserved.
TEST(DungeonClearCcAssistTest, AbortsAfterGraceElapsed)
{
    std::uint32_t out = 0u;
    // Armed at 5000, grace 1000ms: still holding at 5999, aborts at 6000.
    EXPECT_FALSE(ShouldAbortPullForCc(true, 5000u, 5999u, 1000u, out));
    EXPECT_EQ(out, 5000u);
    EXPECT_TRUE(ShouldAbortPullForCc(true, 5000u, 6000u, 1000u, out));
    EXPECT_EQ(out, 5000u);
}

// A micro-CC that clears within the grace re-arms fresh next time, so a flicker
// never accumulates toward an abort.
TEST(DungeonClearCcAssistTest, FlickerDoesNotAccumulate)
{
    std::uint32_t out = 0u;
    // Impaired at 5000 (arm), clears at 5500 (latch -> 0), impaired again at 5800
    // re-arms at 5800 — not at the original 5000 — so the grace restarts.
    EXPECT_FALSE(ShouldAbortPullForCc(true, 0u, 5000u, 1000u, out));
    EXPECT_EQ(out, 5000u);
    EXPECT_FALSE(ShouldAbortPullForCc(false, out, 5500u, 1000u, out));
    EXPECT_EQ(out, 0u);
    EXPECT_FALSE(ShouldAbortPullForCc(true, out, 5800u, 1000u, out));
    EXPECT_EQ(out, 5800u);
    // 5800 + 1000 = 6800 is when it would abort, proving the 5000 spell didn't count.
    EXPECT_FALSE(ShouldAbortPullForCc(true, out, 6700u, 1000u, out));
    EXPECT_TRUE(ShouldAbortPullForCc(true, out, 6800u, 1000u, out));
}

// Zero grace aborts on the very first impaired tick.
TEST(DungeonClearCcAssistTest, ZeroGraceAbortsImmediately)
{
    std::uint32_t out = 0u;
    EXPECT_TRUE(ShouldAbortPullForCc(true, 0u, 5000u, 0u, out));
}

// Latching at now == 0 stores 1, not 0 (0 means "clear"), so a tank impaired at
// the very first millisecond still latches and can abort.
TEST(DungeonClearCcAssistTest, ZeroNowLatchesToOne)
{
    std::uint32_t out = 99u;
    EXPECT_FALSE(ShouldAbortPullForCc(true, 0u, 0u, 1000u, out));
    EXPECT_EQ(out, 1u);
}

// ---------------------------------------------------------------------------
// ShouldDropPullVerdict — the no-target verdict-drop grace gate.
// ---------------------------------------------------------------------------
using DungeonClearMath::ShouldDropPullVerdict;

// Target present: never drop, latch cleared to 0 (even from a stale value).
TEST(DungeonClearVerdictGraceTest, PresentClearsLatch)
{
    std::uint32_t out = 12345u;
    EXPECT_FALSE(ShouldDropPullVerdict(true, 10000u, 20000u, 1500u, out));
    EXPECT_EQ(out, 0u);
}

// First null tick arms the latch (to `now`) but does not yet drop the verdict.
TEST(DungeonClearVerdictGraceTest, FirstNullTickArmsButHolds)
{
    std::uint32_t out = 0u;
    EXPECT_FALSE(ShouldDropPullVerdict(false, 0u, 5000u, 1500u, out));
    EXPECT_EQ(out, 5000u);
}

// A target lost continuously past the grace drops the verdict; latch preserved.
TEST(DungeonClearVerdictGraceTest, DropsAfterGraceElapsed)
{
    std::uint32_t out = 0u;
    // Armed at 5000, grace 1500ms: still holding at 6499, drops at 6500.
    EXPECT_FALSE(ShouldDropPullVerdict(false, 5000u, 6499u, 1500u, out));
    EXPECT_EQ(out, 5000u);
    EXPECT_TRUE(ShouldDropPullVerdict(false, 5000u, 6500u, 1500u, out));
    EXPECT_EQ(out, 5000u);
}

// A transient null (door-veto flicker, cache mid-rebuild) that resolves within
// the grace re-arms fresh next time, so flickers never accumulate to a drop.
TEST(DungeonClearVerdictGraceTest, FlickerDoesNotAccumulate)
{
    std::uint32_t out = 0u;
    // Lost at 5000 (arm), present again at 5400 (latch -> 0), lost again at 5800
    // re-arms at 5800 — not at the original 5000 — so the grace restarts.
    EXPECT_FALSE(ShouldDropPullVerdict(false, 0u, 5000u, 1500u, out));
    EXPECT_EQ(out, 5000u);
    EXPECT_FALSE(ShouldDropPullVerdict(true, out, 5400u, 1500u, out));
    EXPECT_EQ(out, 0u);
    EXPECT_FALSE(ShouldDropPullVerdict(false, out, 5800u, 1500u, out));
    EXPECT_EQ(out, 5800u);
    // 5800 + 1500 = 7300 is when it drops, proving the 5000 loss didn't count.
    EXPECT_FALSE(ShouldDropPullVerdict(false, out, 7299u, 1500u, out));
    EXPECT_TRUE(ShouldDropPullVerdict(false, out, 7300u, 1500u, out));
}

// Zero grace drops on the very first null tick (the pre-grace behavior).
TEST(DungeonClearVerdictGraceTest, ZeroGraceDropsImmediately)
{
    std::uint32_t out = 0u;
    EXPECT_TRUE(ShouldDropPullVerdict(false, 0u, 5000u, 0u, out));
}

// Latching at now == 0 stores 1, not 0 (0 means "present"), so a target lost at
// the very first millisecond still latches and can drop.
TEST(DungeonClearVerdictGraceTest, ZeroNowLatchesToOne)
{
    std::uint32_t out = 99u;
    EXPECT_FALSE(ShouldDropPullVerdict(false, 0u, 0u, 1500u, out));
    EXPECT_EQ(out, 1u);
}
