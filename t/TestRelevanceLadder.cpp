/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"

// Executable form of the trigger relevance ladder (arch-review F3). Pins the
// ordering both strategies depend on and documents every same-value tie with the
// partition (role / engine / map / anchor-kind) that legitimizes it, so a future
// edit that reorders a rung — or lands a new feature on an occupied one — trips a
// red test instead of silently depending on trigger registration order.

// --- non-combat driving ladder, top to bottom ----------------------------

TEST(DungeonClearRelevanceTest, NonCombatLadderStrictlyDescends)
{
    // Every strict > below is an ordering the strategy relies on. Grouped as the
    // ladder reads top-down; ties are asserted separately (partitioned) below.
    EXPECT_GT(DcRel::Chat,            DcRel::LootRollPending);
    EXPECT_GT(DcRel::LootRollPending, DcRel::DoorReopened);
    EXPECT_GT(DcRel::DoorReopened,    DcRel::AllCleared);
    EXPECT_GT(DcRel::AllCleared,      DcRel::HealReposition);
    EXPECT_GT(DcRel::HealReposition,  DcRel::HakkarSuppressor);
    EXPECT_GT(DcRel::HakkarSuppressor, DcRel::HakkarFlame);
    EXPECT_GT(DcRel::HakkarFlame,     DcRel::Pull);        // tie-broken (was ==)
    EXPECT_GT(DcRel::Pull,            DcRel::HakkarLootBlood);
    EXPECT_GT(DcRel::HakkarLootBlood, DcRel::EventDue);
    EXPECT_GT(DcRel::EventDue,        DcRel::AtBoss);
    EXPECT_GT(DcRel::AtBoss,          DcRel::AssistCamp);
    EXPECT_GT(DcRel::AssistCamp,      DcRel::HoldAtCamp);
    EXPECT_GT(DcRel::HoldAtCamp,      DcRel::NeedsRest);
    EXPECT_GT(DcRel::NeedsRest,       DcRel::RoomTrash);   // tie-broken (was ==)
    EXPECT_GT(DcRel::RoomTrash,       DcRel::BlockingTrash);
    EXPECT_GT(DcRel::BlockingTrash,   DcRel::LeaderAssist);
    EXPECT_GT(DcRel::LeaderAssist,    DcRel::DoorBlocked);
    EXPECT_GT(DcRel::DoorBlocked,     DcRel::Stalled);
    EXPECT_GT(DcRel::Stalled,         DcRel::RoomPreclearHold);
    EXPECT_GT(DcRel::RoomPreclearHold, DcRel::Advance);
    EXPECT_GT(DcRel::Advance,         DcRel::FilterLoot);
}

// --- key invariants named in the strategy comments ------------------------

TEST(DungeonClearRelevanceTest, EventDuePreemptsBossAndDoor)
{
    // A due conditional gate must preempt the boss engage AND the door-blocked stall.
    EXPECT_GT(DcRel::EventDue, DcRel::AtBoss);
    EXPECT_GT(DcRel::EventDue, DcRel::DoorBlocked);
}

TEST(DungeonClearRelevanceTest, LeaderAssistFillsGapBelowOwnEngageScans)
{
    // Leader-assist fills the out-of-sight gap: above advance/stall/door, below the
    // tank's own engage scans so a deliberate visible pull always wins.
    EXPECT_GT(DcRel::LeaderAssist, DcRel::DoorBlocked);
    EXPECT_GT(DcRel::LeaderAssist, DcRel::Stalled);
    EXPECT_GT(DcRel::LeaderAssist, DcRel::Advance);
    EXPECT_LT(DcRel::LeaderAssist, DcRel::BlockingTrash);
    EXPECT_LT(DcRel::LeaderAssist, DcRel::RoomTrash);
    EXPECT_LT(DcRel::LeaderAssist, DcRel::AtBoss);
}

TEST(DungeonClearRelevanceTest, HakkarSuppressorOutranksFlameOutranksBlood)
{
    EXPECT_GT(DcRel::HakkarSuppressor, DcRel::HakkarFlame);
    EXPECT_GT(DcRel::HakkarFlame,      DcRel::HakkarLootBlood);
    // Combat side mirrors the order at the higher band.
    EXPECT_GT(DcRel::HakkarSuppressorCombat, DcRel::HakkarFlameCombat);
    EXPECT_GT(DcRel::HakkarFlameCombat,      DcRel::HakkarLootBloodCombat);
}

// --- combat engine ladder -------------------------------------------------

TEST(DungeonClearRelevanceTest, CombatLadderStrictlyDescends)
{
    EXPECT_GT(DcRel::HakkarSuppressorCombat, DcRel::HakkarFlameCombat);
    EXPECT_GT(DcRel::HakkarFlameCombat,      DcRel::HakkarLootBloodCombat);
    EXPECT_GT(DcRel::HakkarLootBloodCombat,  DcRel::PullManeuver);
    EXPECT_GT(DcRel::PullManeuver,           DcRel::HealReposition);
    EXPECT_GT(DcRel::HealReposition,         DcRel::AssistCampCombat);
    EXPECT_GT(DcRel::AssistCampCombat,       DcRel::RegroupCombat);
}

// --- ties, each with the partition that makes them safe -------------------

TEST(DungeonClearRelevanceTest, PartitionedTiesAreEqualByDesign)
{
    // Distinct trigger conditions (keyword vs death), both terminal.
    EXPECT_FLOAT_EQ(DcRel::Chat, DcRel::PartyDied);
    // Mutually exclusive by the anchor-kind check in each trigger (boss vs objective).
    EXPECT_FLOAT_EQ(DcRel::AtBoss, DcRel::AtObjective);
    // Leader-only vs follower-only.
    EXPECT_FLOAT_EQ(DcRel::BlockingTrash, DcRel::FollowTank);
    // Combat: leader-only (drag) vs follower-only (pin) — role peers.
    EXPECT_FLOAT_EQ(DcRel::PullManeuver, DcRel::StayAtCamp);
}

TEST(DungeonClearRelevanceTest, HealRepositionAboveLeaderDriversIsRolePartitioned)
{
    // In BOTH engines heal-reposition (41, healer-only) sits above the leader-only
    // pull/hakkar/at-boss drivers. It never contends because of the healer-vs-leader
    // role split — asserted so a future non-healer trigger at 41 is caught.
    EXPECT_GT(DcRel::HealReposition, DcRel::Pull);
    EXPECT_GT(DcRel::HealReposition, DcRel::HakkarSuppressor);
    EXPECT_GT(DcRel::HealReposition, DcRel::AtBoss);
}

TEST(DungeonClearRelevanceTest, PreviouslyTiedRungsAreNowStrictlyOrdered)
{
    // The two ties that were NOT role/map/anchor partitioned are broken so
    // ordering never depends on trigger registration order.
    EXPECT_NE(DcRel::HakkarFlame, DcRel::Pull);
    EXPECT_NE(DcRel::NeedsRest,   DcRel::RoomTrash);
}
