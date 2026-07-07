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

// ---------------------------------------------------------------------------
// ShouldRollInForLeeroy — the Leeroy roll-in scout-lag release gate.
// ---------------------------------------------------------------------------
using DungeonClearMath::ShouldRollInForLeeroy;

// Only decision == 1 (standing Leeroy) releases; 0 (none, still scouting) and
// 2 (Advanced — the camp machinery owns the party) hold the lag even in range.
TEST(DungeonClearRollInTest, OnlyLeeroyDecisionRollsIn)
{
    EXPECT_FALSE(ShouldRollInForLeeroy(0u, true, 10.0f, 20.0f, 8.0f));
    EXPECT_TRUE(ShouldRollInForLeeroy(1u, true, 10.0f, 20.0f, 8.0f));
    EXPECT_FALSE(ShouldRollInForLeeroy(2u, true, 10.0f, 20.0f, 8.0f));
}

// A dead/unresolvable verdict target never rolls in, regardless of distance.
TEST(DungeonClearRollInTest, DeadTargetNeverRollsIn)
{
    EXPECT_FALSE(ShouldRollInForLeeroy(1u, false, 10.0f, 20.0f, 8.0f));
    EXPECT_FALSE(ShouldRollInForLeeroy(1u, false, 0.0f, 20.0f, 8.0f));
}

// Boundary is inclusive at commitRange + lead; just beyond it holds.
TEST(DungeonClearRollInTest, BoundaryAtCommitPlusLead)
{
    EXPECT_TRUE(ShouldRollInForLeeroy(1u, true, 28.0f, 20.0f, 8.0f));
    EXPECT_FALSE(ShouldRollInForLeeroy(1u, true, 28.1f, 20.0f, 8.0f));
}

// lead == 0 releases only once the tank reaches the commit range itself.
TEST(DungeonClearRollInTest, ZeroLeadReleasesOnlyAtCommitRange)
{
    EXPECT_FALSE(ShouldRollInForLeeroy(1u, true, 20.5f, 20.0f, 0.0f));
    EXPECT_TRUE(ShouldRollInForLeeroy(1u, true, 20.0f, 20.0f, 0.0f));
    EXPECT_TRUE(ShouldRollInForLeeroy(1u, true, 15.0f, 20.0f, 0.0f));
}

// Degenerate distances (tank on top of / past the pack) still roll in — the
// tank could not be more committed.
TEST(DungeonClearRollInTest, ZeroOrNegativeDistanceRollsIn)
{
    EXPECT_TRUE(ShouldRollInForLeeroy(1u, true, 0.0f, 20.0f, 8.0f));
    EXPECT_TRUE(ShouldRollInForLeeroy(1u, true, -1.0f, 20.0f, 8.0f));
}

// --- FindTrailRejoin (breadcrumb truncate-don't-clear) --------------------

using DungeonClearMath::FindTrailRejoin;
using DungeonClearMath::TrailRejoinNone;

// Helper: build a straight-line trail of `n` crumbs spaced 4yd on +X at z=0.
static std::vector<Position> MakeLine(std::size_t n)
{
    std::vector<Position> v;
    for (std::size_t i = 0; i < n; ++i)
        v.emplace_back(static_cast<float>(i) * 4.0f, 0.0f, 0.0f, 0.0f);
    return v;
}

TEST(DungeonClearTrailRejoinTest, RejoinsAtExactCampCrumb)
{
    // 7 crumbs at x = 0,4,..,24. Bot stands on the LAST crumb (x=24): even though
    // crumb 5 (x=20) is also within 6yd behind it, latest-wins returns crumb 6
    // (the standing-at-camp -> rejoin-the-camp-crumb case).
    std::vector<Position> trail = MakeLine(7);
    Position const cur(24.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(FindTrailRejoin(trail, cur, 6.0f), 6u);
}

TEST(DungeonClearTrailRejoinTest, RejoinsAtNearestWithinRadius)
{
    // 2yd past the last crumb (x=24): within 6yd of crumbs 5 (x=20) and 6
    // (x=24); latest-wins picks crumb 6.
    std::vector<Position> trail = MakeLine(7);
    Position const cur(26.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(FindTrailRejoin(trail, cur, 6.0f), 6u);
}

TEST(DungeonClearTrailRejoinTest, LatestWinsOnSelfLoop)
{
    // Trail loops back near its own start: an early crumb and a late crumb both
    // sit at the origin. Rejoin must pick the LATEST (index 6), not index 0.
    std::vector<Position> trail;
    trail.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);     // 0  (start)
    trail.emplace_back(10.0f, 0.0f, 0.0f, 0.0f);    // 1
    trail.emplace_back(20.0f, 0.0f, 0.0f, 0.0f);    // 2
    trail.emplace_back(20.0f, 10.0f, 0.0f, 0.0f);   // 3
    trail.emplace_back(10.0f, 10.0f, 0.0f, 0.0f);   // 4
    trail.emplace_back(0.0f, 10.0f, 0.0f, 0.0f);    // 5
    trail.emplace_back(1.0f, 1.0f, 0.0f, 0.0f);     // 6  (back near start)
    Position const cur(0.5f, 0.5f, 0.0f, 0.0f);
    EXPECT_EQ(FindTrailRejoin(trail, cur, 6.0f), 6u);
}

TEST(DungeonClearTrailRejoinTest, NoRejoinOnTrueTeleport)
{
    std::vector<Position> trail = MakeLine(10);     // all near the X axis
    Position const cur(500.0f, 500.0f, 0.0f, 0.0f); // far away -> teleport
    EXPECT_EQ(FindTrailRejoin(trail, cur, 6.0f), TrailRejoinNone);
}

TEST(DungeonClearTrailRejoinTest, RadiusBoundaryIsInclusive)
{
    std::vector<Position> trail = MakeLine(3);      // crumbs at x = 0,4,8
    // Exactly 6yd from crumb 2 (x=8) -> at the boundary -> rejoins (<=).
    Position const cur(14.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(FindTrailRejoin(trail, cur, 6.0f), 2u);
    // Just past the boundary from any crumb -> no rejoin.
    Position const past(14.01f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(FindTrailRejoin(trail, past, 6.0f), TrailRejoinNone);
}

TEST(DungeonClearTrailRejoinTest, VerticalSeparationIsNotARejoin)
{
    // A crumb directly below the bot (same X/Y, 20yd down) must NOT count: the
    // rejoin test is 3D so a different floor never rejoins.
    std::vector<Position> trail;
    trail.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
    Position const cur(0.0f, 0.0f, 20.0f, 0.0f);
    EXPECT_EQ(FindTrailRejoin(trail, cur, 6.0f), TrailRejoinNone);
}

TEST(DungeonClearTrailRejoinTest, EmptyTrailAndBadRadius)
{
    std::vector<Position> empty;
    Position const cur(0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(FindTrailRejoin(empty, cur, 6.0f), TrailRejoinNone);
    // Non-positive radius never rejoins even on a coincident crumb.
    std::vector<Position> one = MakeLine(1);
    EXPECT_EQ(FindTrailRejoin(one, cur, 0.0f), TrailRejoinNone);
}

// --- ShouldPlantEarly (turn-and-plant drag-back gate) ---------------------

using DungeonClearMath::ShouldPlantEarly;

// Helper: N attackers all at the same distance.
static std::vector<float> Attackers(std::size_t n, float dist)
{
    return std::vector<float>(n, dist);
}

// Pack gathered (all within glue) for the required ticks past half the leg:
// arms on tick 1, plants on tick 2 (glueTicksNeeded == 2).
TEST(DungeonClearPlantTest, GatheredPackPlantsAfterDebounce)
{
    std::uint32_t ticks = 0;
    std::vector<float> const dists = Attackers(3, 4.0f);
    // legStartDist 40, distToCamp 10 (well past half) -> qualifies each tick.
    EXPECT_FALSE(ShouldPlantEarly(dists, 6.0f, 2u, false, 10.0f, 40.0f, ticks));
    EXPECT_EQ(ticks, 1u);
    EXPECT_TRUE(ShouldPlantEarly(dists, 6.0f, 2u, false, 10.0f, 40.0f, ticks));
    EXPECT_EQ(ticks, 2u);
}

// A single straggler outside the glue radius vetoes the plant and resets the
// debounce latch.
TEST(DungeonClearPlantTest, StragglerVetoesAndResets)
{
    std::uint32_t ticks = 1;  // armed from a prior gathered tick
    std::vector<float> dists = Attackers(2, 4.0f);
    dists.push_back(9.0f);    // one mob still 9yd back
    EXPECT_FALSE(ShouldPlantEarly(dists, 6.0f, 2u, false, 10.0f, 40.0f, ticks));
    EXPECT_EQ(ticks, 0u);
}

// LOS-break pulls never plant short (must reach the corner); latch reset.
TEST(DungeonClearPlantTest, LosPullNeverPlants)
{
    std::uint32_t ticks = 5;
    std::vector<float> const dists = Attackers(3, 2.0f);
    EXPECT_FALSE(ShouldPlantEarly(dists, 6.0f, 2u, true, 5.0f, 40.0f, ticks));
    EXPECT_EQ(ticks, 0u);
}

// Less than half the return leg covered vetoes (distToCamp > legStartDist/2).
TEST(DungeonClearPlantTest, FirstHalfOfLegRequired)
{
    std::uint32_t ticks = 0;
    std::vector<float> const dists = Attackers(3, 3.0f);
    // legStartDist 40 -> half is 20; distToCamp 25 is too early.
    EXPECT_FALSE(ShouldPlantEarly(dists, 6.0f, 2u, false, 25.0f, 40.0f, ticks));
    EXPECT_EQ(ticks, 0u);
    // Exactly at half qualifies (inclusive).
    EXPECT_FALSE(ShouldPlantEarly(dists, 6.0f, 2u, false, 20.0f, 40.0f, ticks));
    EXPECT_EQ(ticks, 1u);
}

// Nothing chasing (evade/fizzle): never a plant, latch cleared.
TEST(DungeonClearPlantTest, EmptyAttackersNeverPlants)
{
    std::uint32_t ticks = 1;
    std::vector<float> const none;
    EXPECT_FALSE(ShouldPlantEarly(none, 6.0f, 2u, false, 5.0f, 40.0f, ticks));
    EXPECT_EQ(ticks, 0u);
}

// An unstamped leg (legStartDist <= 0) can never qualify.
TEST(DungeonClearPlantTest, NoLegStartNeverPlants)
{
    std::uint32_t ticks = 0;
    std::vector<float> const dists = Attackers(2, 2.0f);
    EXPECT_FALSE(ShouldPlantEarly(dists, 6.0f, 2u, false, 1.0f, 0.0f, ticks));
    EXPECT_EQ(ticks, 0u);
}

// A noisy gather/break/gather sequence cannot accumulate across the break.
TEST(DungeonClearPlantTest, FlickerDoesNotAccumulate)
{
    std::uint32_t ticks = 0;
    std::vector<float> const tight = Attackers(2, 3.0f);
    std::vector<float> loose = Attackers(1, 3.0f);
    loose.push_back(10.0f);
    EXPECT_FALSE(ShouldPlantEarly(tight, 6.0f, 2u, false, 10.0f, 40.0f, ticks)); // 1
    EXPECT_FALSE(ShouldPlantEarly(loose, 6.0f, 2u, false, 10.0f, 40.0f, ticks)); // reset
    EXPECT_EQ(ticks, 0u);
    EXPECT_FALSE(ShouldPlantEarly(tight, 6.0f, 2u, false, 10.0f, 40.0f, ticks)); // 1 again
    EXPECT_TRUE(ShouldPlantEarly(tight, 6.0f, 2u, false, 10.0f, 40.0f, ticks));  // 2
}

// --- ShouldReleaseFollower (threat-lead window) ---------------------------

using DungeonClearMath::ShouldReleaseFollower;

// Healers release immediately, regardless of an unexpired lead.
TEST(DungeonClearReleaseTest, HealerReleasesImmediately)
{
    EXPECT_TRUE(ShouldReleaseFollower(true, 5000u, 5100u, 1500u, 100.0f, 60.0f));
}

// DPS held inside the lead window, released once it elapses.
TEST(DungeonClearReleaseTest, DpsHeldThenReleased)
{
    // 100ms into a 1500ms lead -> held.
    EXPECT_FALSE(ShouldReleaseFollower(false, 5000u, 5100u, 1500u, 100.0f, 60.0f));
    // Exactly at the lead boundary -> released (inclusive).
    EXPECT_TRUE(ShouldReleaseFollower(false, 5000u, 6500u, 1500u, 100.0f, 60.0f));
    // Well past -> released.
    EXPECT_TRUE(ShouldReleaseFollower(false, 5000u, 9000u, 1500u, 100.0f, 60.0f));
}

// A tank below the panic HP releases the party early despite the lead.
TEST(DungeonClearReleaseTest, PanicHpBypassesLead)
{
    // 100ms in, but tank at 55% < 60% panic -> release now.
    EXPECT_TRUE(ShouldReleaseFollower(false, 5000u, 5100u, 1500u, 55.0f, 60.0f));
    // Tank healthy -> still held.
    EXPECT_FALSE(ShouldReleaseFollower(false, 5000u, 5100u, 1500u, 80.0f, 60.0f));
    // panicHp 0 disables the bypass: a near-dead tank stays held inside the lead.
    EXPECT_FALSE(ShouldReleaseFollower(false, 5000u, 5100u, 1500u, 1.0f, 0.0f));
}

// leadMs == 0 turns the feature off: DPS release at once.
TEST(DungeonClearReleaseTest, ZeroLeadOff)
{
    EXPECT_TRUE(ShouldReleaseFollower(false, 5000u, 5000u, 0u, 100.0f, 60.0f));
}

// combatSince == 0 (leader not observed in combat): gate is moot, release.
TEST(DungeonClearReleaseTest, NoCombatStampReleases)
{
    EXPECT_TRUE(ShouldReleaseFollower(false, 0u, 5100u, 1500u, 100.0f, 60.0f));
}

// --- Elite weighting: pull weight in thirds of an elite -------------------
//
// The verdict weighs the counted set instead of its raw body count: an elite is a
// full unit (3 thirds), a normal a third (1). The DynPullMob `elite` flag is the
// 8th brace field. Body count (EstimateAggroCount's return) is unchanged — only
// the weightThirdsOut tally and the verdict that compares it to a x3-scaled
// ceiling react to it.

namespace
{
    // Pull weight, in thirds of an elite, of the counted set.
    unsigned Weight(std::vector<DynPullMob> const& m, std::size_t t)
    {
        std::uint32_t w = 0;
        DungeonClearMath::EstimateAggroCount(m, t, kSpread, kAssistR, kZTol,
                                             /*excludeLonePatrollers*/ false,
                                             /*countedOut*/ nullptr, &w);
        return w;
    }
    // The production verdict: weighted tally vs the x3-scaled ceiling.
    bool WeightedAdvanced(std::vector<DynPullMob> const& m, std::size_t t)
    {
        return Weight(m, t) > kCeil * 3u;
    }
}

// All-normal pack: weight == body count (each normal is 1 third).
TEST(DungeonClearEliteWeightTest, AllNormalWeighsOnePerBody)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach, false, /*elite*/ false},
        {3.0f, 0.0f, 0.0f, true,  0u, kReach, false, /*elite*/ false},
        {0.0f, 4.0f, 0.0f, true,  0u, kReach, false, /*elite*/ false}
    };
    EXPECT_EQ(Count(mobs, 0), 3u);
    EXPECT_EQ(Weight(mobs, 0), 3u);  // 3 normals = 3 thirds
}

// All-elite pack: each body weighs 3 thirds.
TEST(DungeonClearEliteWeightTest, AllEliteWeighsThreePerBody)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach, false, /*elite*/ true},
        {3.0f, 0.0f, 0.0f, true,  0u, kReach, false, /*elite*/ true},
        {0.0f, 4.0f, 0.0f, true,  0u, kReach, false, /*elite*/ true}
    };
    EXPECT_EQ(Count(mobs, 0), 3u);
    EXPECT_EQ(Weight(mobs, 0), 9u);  // 3 elites = 9 thirds
}

// The motivating fix: a big room of NORMAL trash that body-counts ABOVE the
// ceiling (6 > 5) now weighs only 2 elite-equivalents (6 thirds) -> stays Leeroy.
TEST(DungeonClearEliteWeightTest, LargeNormalRoomStaysLeeroy)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach, false, false},
        {3.0f, 0.0f, 0.0f, true,  0u, kReach, false, false},
        {6.0f, 0.0f, 0.0f, true,  0u, kReach, false, false},
        {9.0f, 0.0f, 0.0f, true,  0u, kReach, false, false},
        {12.0f, 0.0f, 0.0f, true, 0u, kReach, false, false},
        {15.0f, 0.0f, 0.0f, true, 0u, kReach, false, false}
    };
    EXPECT_EQ(Count(mobs, 0), 6u);             // six bodies
    EXPECT_EQ(Weight(mobs, 0), 6u);            // = 2 elite-equiv
    EXPECT_FALSE(WeightedAdvanced(mobs, 0));   // 6 <= ceiling 15 thirds -> Leeroy
}

// Six ELITE bodies clear the same ceiling (18 > 15 thirds) -> Advanced, as before.
TEST(DungeonClearEliteWeightTest, LargeEliteRoomGoesAdvanced)
{
    std::vector<DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach, false, true},
        {3.0f, 0.0f, 0.0f, true,  0u, kReach, false, true},
        {6.0f, 0.0f, 0.0f, true,  0u, kReach, false, true},
        {9.0f, 0.0f, 0.0f, true,  0u, kReach, false, true},
        {12.0f, 0.0f, 0.0f, true, 0u, kReach, false, true},
        {15.0f, 0.0f, 0.0f, true, 0u, kReach, false, true}
    };
    EXPECT_EQ(Count(mobs, 0), 6u);
    EXPECT_EQ(Weight(mobs, 0), 18u);
    EXPECT_TRUE(WeightedAdvanced(mobs, 0));    // 18 > 15 thirds -> Advanced
}

// Just past the ceiling on weight: six elites (18) is Advanced, five (15) is the
// boundary (15 == ceiling, not >) so Leeroy — confirms the > comparison and scale.
TEST(DungeonClearEliteWeightTest, FiveElitesIsTheLeeroyBoundary)
{
    std::vector<DynPullMob> five = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach, false, true},
        {3.0f, 0.0f, 0.0f, true,  0u, kReach, false, true},
        {6.0f, 0.0f, 0.0f, true,  0u, kReach, false, true},
        {9.0f, 0.0f, 0.0f, true,  0u, kReach, false, true},
        {12.0f, 0.0f, 0.0f, true, 0u, kReach, false, true}
    };
    EXPECT_EQ(Weight(five, 0), 15u);
    EXPECT_FALSE(WeightedAdvanced(five, 0));    // 15 == ceiling -> Leeroy
}

// --- Patrol-wait: EstimateAggroCount lone-patroller exclusion --------------

// Helper: the reduced pass (lone patrollers excluded).
static unsigned CountReduced(std::vector<DungeonClearMath::DynPullMob> const& m,
                             std::size_t t)
{
    return DungeonClearMath::EstimateAggroCount(m, t, kSpread, kAssistR, kZTol,
                                                /*excludeLonePatrollers*/ true);
}

// A lone patroller (packId 0) that tips a clean 2-mob Leeroy over the ceiling is
// dropped in the reduced pass: full counts it, reduced does not.
TEST(DungeonClearPatrolWaitTest, LonePatrollerDropsInReducedPass)
{
    // Target pair (count 2) + four more bodies in reach, one of them a lone
    // patroller. Full = 6 (> ceiling 5 => Advanced); reduced without the patroller
    // = 5 (<= ceiling => clean Leeroy) => patrol-contended.
    std::vector<DungeonClearMath::DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach},               // 0 target
        {3.0f, 0.0f, 0.0f, true,  0u, kReach},               // 1 packmate
        {6.0f, 0.0f, 0.0f, true,  0u, kReach},               // 2
        {9.0f, 0.0f, 0.0f, true,  0u, kReach},               // 3
        {12.0f, 0.0f, 0.0f, true, 0u, kReach},               // 4
        {15.0f, 0.0f, 0.0f, true, 0u, kReach, /*patrol*/ true} // 5 lone patroller
    };
    EXPECT_EQ(Count(mobs, 0), 6u);
    EXPECT_EQ(CountReduced(mobs, 0), 5u);
}

// A patroller that shares an atomic pack (packId != 0) is NOT droppable — you
// cannot wait out half a formation — so the reduced pass still counts it.
TEST(DungeonClearPatrolWaitTest, FormationPatrollerStaysInReducedPass)
{
    // Mob 5 is a patroller but linked (packId 7) to mob 4; both come as a unit.
    std::vector<DungeonClearMath::DynPullMob> mobs = {
        {0.0f, 0.0f, 0.0f, false, 0u, kReach},                 // 0 target
        {3.0f, 0.0f, 0.0f, true,  0u, kReach},                 // 1
        {6.0f, 0.0f, 0.0f, true,  0u, kReach},                 // 2
        {9.0f, 0.0f, 0.0f, true,  0u, kReach},                 // 3
        {12.0f, 0.0f, 0.0f, true, 7u, kReach},                 // 4 formation
        {15.0f, 0.0f, 0.0f, true, 7u, kReach, /*patrol*/ true} // 5 patroller, linked
    };
    EXPECT_EQ(Count(mobs, 0), 6u);
    EXPECT_EQ(CountReduced(mobs, 0), 6u);  // linked patroller survives
}

// --- ShouldWaitForPatrol (the patrol-wait latch gate) ---------------------

using DungeonClearMath::ShouldWaitForPatrol;

// Not contended (full at/under ceiling): never wait, latch cleared.
TEST(DungeonClearPatrolGateTest, NotContendedProceeds)
{
    std::uint32_t since = 1234u;
    // full 5 <= ceiling 5 -> not contended.
    EXPECT_FALSE(ShouldWaitForPatrol(5u, 5u, 5u, since, 9000u, 8000u, since));
    EXPECT_EQ(since, 0u);
}

// Reduced ALSO over the ceiling: the patrol isn't the sole cause -> proceed
// (commit Advanced), don't wait.
TEST(DungeonClearPatrolGateTest, ReducedStillOverIsNotPatrolContended)
{
    std::uint32_t since = 0u;
    EXPECT_FALSE(ShouldWaitForPatrol(8u, 6u, 5u, since, 1000u, 8000u, since));
    EXPECT_EQ(since, 0u);
}

// Contended (full over, reduced under): arm the latch and hold until the budget.
TEST(DungeonClearPatrolGateTest, ContendedWaitsThenTimesOut)
{
    std::uint32_t since = 0u;
    // First contended tick: arm at now=1000, hold.
    EXPECT_TRUE(ShouldWaitForPatrol(6u, 5u, 5u, since, 1000u, 8000u, since));
    EXPECT_EQ(since, 1000u);
    // Still inside the 8s budget -> keep waiting.
    EXPECT_TRUE(ShouldWaitForPatrol(6u, 5u, 5u, since, 5000u, 8000u, since));
    EXPECT_EQ(since, 1000u);
    // At/after the budget -> proceed (latch stays armed so it can't re-wait).
    EXPECT_FALSE(ShouldWaitForPatrol(6u, 5u, 5u, since, 9000u, 8000u, since));
    EXPECT_EQ(since, 1000u);
}

// The patrol leaving (reduced==full, both fine) clears an armed latch and proceeds.
TEST(DungeonClearPatrolGateTest, PatrolLeavingClearsLatch)
{
    std::uint32_t since = 1000u;  // armed
    EXPECT_FALSE(ShouldWaitForPatrol(4u, 4u, 5u, since, 3000u, 8000u, since));
    EXPECT_EQ(since, 0u);
}

// Ceiling boundary: contended needs full strictly above and reduced at/under.
TEST(DungeonClearPatrolGateTest, CeilingBoundary)
{
    std::uint32_t since = 0u;
    // full 6 > 5 and reduced 5 <= 5 -> contended -> wait.
    EXPECT_TRUE(ShouldWaitForPatrol(6u, 5u, 5u, since, 100u, 8000u, since));
    // full == ceiling -> not contended.
    since = 0u;
    EXPECT_FALSE(ShouldWaitForPatrol(5u, 4u, 5u, since, 100u, 8000u, since));
}

// waitMs == 0 proceeds immediately even when contended.
TEST(DungeonClearPatrolGateTest, ZeroBudgetProceeds)
{
    std::uint32_t since = 0u;
    EXPECT_FALSE(ShouldWaitForPatrol(6u, 5u, 5u, since, 100u, 0u, since));
}

// --- ShouldHandoffFizzledPull (advanced-pull engage-fizzle latch) ----------
using DungeonClearMath::ShouldHandoffFizzledPull;

TEST(DungeonClearFizzleTest, NonFizzleClearsCount)
{
    // Pack died or is still being fought (pulledAliveIdle == false): clear latch.
    std::uint32_t count = 5u;
    EXPECT_FALSE(ShouldHandoffFizzledPull(/*aliveIdle*/ false, /*same*/ false, 2u, count));
    EXPECT_EQ(count, 0u);
}

TEST(DungeonClearFizzleTest, NewTargetRestartsAtOne)
{
    // A fizzle of a different pack than the latch holds restarts the run at 1.
    std::uint32_t count = 4u;
    EXPECT_FALSE(ShouldHandoffFizzledPull(/*aliveIdle*/ true, /*same*/ false, 2u, count));
    EXPECT_EQ(count, 1u);
}

TEST(DungeonClearFizzleTest, SameTargetRunReachesHandoff)
{
    // Two consecutive same-pack fizzles reach DC_PULL_FIZZLE_MAX (2) -> handoff.
    std::uint32_t count = 0u;
    EXPECT_FALSE(ShouldHandoffFizzledPull(true, false, 2u, count));  // fizzle #1 (new)
    EXPECT_EQ(count, 1u);
    EXPECT_TRUE(ShouldHandoffFizzledPull(true, true, 2u, count));    // fizzle #2 (same)
    EXPECT_EQ(count, 2u);
}

TEST(DungeonClearFizzleTest, InterruptingKillResetsRun)
{
    // A same-pack fizzle, then the pack dies/gets fought (reset), then it fizzles
    // again as a "new" run — must NOT hand off on the single post-reset fizzle.
    std::uint32_t count = 1u;                                        // one prior fizzle
    EXPECT_FALSE(ShouldHandoffFizzledPull(false, false, 2u, count)); // fought -> reset
    EXPECT_EQ(count, 0u);
    EXPECT_FALSE(ShouldHandoffFizzledPull(true, false, 2u, count));  // fresh fizzle
    EXPECT_EQ(count, 1u);
}

TEST(DungeonClearFizzleTest, ZeroMaxHandsOffOnFirstFizzle)
{
    std::uint32_t count = 0u;
    EXPECT_TRUE(ShouldHandoffFizzledPull(true, false, 0u, count));
}
