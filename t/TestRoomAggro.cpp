/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"

// --- Find -----------------------------------------------------------------

TEST(RoomAggroRegistryTest, FindsFlaggedBoss)
{
    RoomAggroBoss const* b = RoomAggroRegistry::Find(557, 18341);  // Pandemonius
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->bossEntry, 18341u);
    EXPECT_FLOAT_EQ(b->radius, 70.0f);
}

TEST(RoomAggroRegistryTest, UnflaggedBossReturnsNull)
{
    // Right map, wrong entry.
    EXPECT_EQ(RoomAggroRegistry::Find(557, 99999), nullptr);
    // Right entry, wrong map (registry is keyed by both).
    EXPECT_EQ(RoomAggroRegistry::Find(0, 18341), nullptr);
}

TEST(RoomAggroRegistryTest, MultiWingMapIsKeyedByEntry)
{
    // SM Cathedral shares map 189 with other wings; only the cathedral bosses
    // are flagged.
    EXPECT_NE(RoomAggroRegistry::Find(189, 3976), nullptr);  // Mograine
    EXPECT_NE(RoomAggroRegistry::Find(189, 3977), nullptr);  // Whitemane
    EXPECT_EQ(RoomAggroRegistry::Find(189, 6487), nullptr);  // (some other 189 npc)
}

// Scholomance's linked pair are both flagged so the room-trash value tracks the
// room around the live boss (Vectus) AND excludes the off-roster partner
// (Marduk) from being chased as a faction-15 student. Unlike every other
// room-aggro boss, their room is scoped to an EXPLICIT whitelist (Scholomance
// Students, 10475) because the whole chamber stands neutral until struck — the
// whitelist both keeps unrelated hostile packs out of "the room" and signals the
// value to clear the neutral students (see DungeonClearRoomTrashValue).
TEST(RoomAggroRegistryTest, ScholomanceLinkedPairBothFlagged)
{
    RoomAggroBoss const* vectus = RoomAggroRegistry::Find(289, 10432);
    RoomAggroBoss const* marduk = RoomAggroRegistry::Find(289, 10433);
    ASSERT_NE(vectus, nullptr);
    ASSERT_NE(marduk, nullptr);
    EXPECT_EQ(RoomAggroRegistry::Find(289, 10475), nullptr);  // Student is room, not boss

    // Both rows carry the student-only whitelist.
    ASSERT_EQ(vectus->memberEntries.size(), 1u);
    EXPECT_EQ(vectus->memberEntries[0], 10475u);
    EXPECT_EQ(marduk->memberEntries, vectus->memberEntries);

    // The whitelist scopes the room: a Scholomance Student inside the radius and
    // outside the boss sphere is room trash; an unrelated hostile pack (e.g. a
    // Diseased Ghoul, 10495) at the same distance is NOT.
    EXPECT_TRUE(RoomAggroRegistry::IsRoomTrash(*vectus, 10475, 45.0f, 31.0f));
    EXPECT_FALSE(RoomAggroRegistry::IsRoomTrash(*vectus, 10495, 45.0f, 31.0f));
}

// --- IsMemberEntry --------------------------------------------------------

TEST(RoomAggroRegistryTest, EmptyWhitelistMatchesAnyEntry)
{
    RoomAggroBoss boss{557, 18341, 70.0f, {}};
    EXPECT_TRUE(RoomAggroRegistry::IsMemberEntry(boss, 18309));
    EXPECT_TRUE(RoomAggroRegistry::IsMemberEntry(boss, 12345));
}

TEST(RoomAggroRegistryTest, NonEmptyWhitelistMatchesOnlyListed)
{
    RoomAggroBoss boss{557, 18341, 70.0f, {18309, 18311, 18313}};
    EXPECT_TRUE(RoomAggroRegistry::IsMemberEntry(boss, 18311));
    EXPECT_FALSE(RoomAggroRegistry::IsMemberEntry(boss, 99999));
}

// --- IsRoomTrash (radius + boss-aggro-sphere exclusion) -------------------

TEST(RoomAggroRegistryTest, RoomTrashInsideRadiusOutsideBossSphere)
{
    RoomAggroBoss boss{557, 18341, 70.0f, {}};
    // 30yd from the boss, boss aggro sphere 12yd -> clearable room trash.
    EXPECT_TRUE(RoomAggroRegistry::IsRoomTrash(boss, 18309, 30.0f, 12.0f));
}

TEST(RoomAggroRegistryTest, RoomTrashBeyondRadiusExcluded)
{
    RoomAggroBoss boss{557, 18341, 70.0f, {}};
    EXPECT_FALSE(RoomAggroRegistry::IsRoomTrash(boss, 18309, 80.0f, 12.0f));
}

TEST(RoomAggroRegistryTest, RoomTrashInsideBossSphereExcluded)
{
    RoomAggroBoss boss{557, 18341, 70.0f, {}};
    // Glued to the boss (8yd, sphere 12yd) -> comes with the boss, not counted.
    EXPECT_FALSE(RoomAggroRegistry::IsRoomTrash(boss, 18309, 8.0f, 12.0f));
}

TEST(RoomAggroRegistryTest, RoomTrashNonMemberExcluded)
{
    RoomAggroBoss boss{557, 18341, 70.0f, {18309}};
    // In range and outside the sphere, but not on the whitelist.
    EXPECT_FALSE(RoomAggroRegistry::IsRoomTrash(boss, 99999, 30.0f, 12.0f));
}

// --- NeedsRoomAggroSkirt (the skirt chooser) ------------------------------
//
// Boss centred at the origin, aggro sphere radius 10. The bot approaches a
// target and we ask whether the straight 2D chord clips the sphere.

TEST(RoomAggroSkirtTest, ChordCrossingSphereNeedsSkirt)
{
    // Bot at (-30, 5), target at (30, 5): the chord runs left-to-right just 5yd
    // above the boss at the origin — well inside the 10yd sphere.
    EXPECT_TRUE(DcEngageGeometry::NeedsRoomAggroSkirt(
        -30.0f, 5.0f, 30.0f, 5.0f, 0.0f, 0.0f, 10.0f));
}

TEST(RoomAggroSkirtTest, ChordClearOfSphereGoesDirect)
{
    // Same span but 20yd above the boss: the chord never enters the 10yd sphere.
    EXPECT_FALSE(DcEngageGeometry::NeedsRoomAggroSkirt(
        -30.0f, 20.0f, 30.0f, 20.0f, 0.0f, 0.0f, 10.0f));
}

TEST(RoomAggroSkirtTest, TargetOnNearSideGoesDirect)
{
    // Bot and target both well to one side of the boss; the segment's closest
    // approach to the origin is the target endpoint at 25yd, outside the sphere.
    EXPECT_FALSE(DcEngageGeometry::NeedsRoomAggroSkirt(
        -40.0f, 0.0f, -25.0f, 0.0f, 0.0f, 0.0f, 10.0f));
}

TEST(RoomAggroSkirtTest, BotInsidePaddedSphereStillSkirts)
{
    // Recovery case: the bot is standing 4yd from the boss centre, inside the
    // 10yd sphere. The chord's closest point (the bot endpoint) is within the
    // radius, so a skirt/exit waypoint is still demanded — never "go direct".
    EXPECT_TRUE(DcEngageGeometry::NeedsRoomAggroSkirt(
        4.0f, 0.0f, 30.0f, 0.0f, 0.0f, 0.0f, 10.0f));
}

TEST(RoomAggroSkirtTest, DegenerateZeroLengthChord)
{
    // Bot and target coincide far from the boss -> the (point) chord is 50yd out,
    // outside the sphere -> no skirt. (Exercises the A==B segment path.)
    EXPECT_FALSE(DcEngageGeometry::NeedsRoomAggroSkirt(
        50.0f, 0.0f, 50.0f, 0.0f, 0.0f, 0.0f, 10.0f));
    // And the same coincident point sitting ON the boss -> inside -> skirt.
    EXPECT_TRUE(DcEngageGeometry::NeedsRoomAggroSkirt(
        1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 10.0f));
}

TEST(RoomAggroSkirtTest, NonPositiveRadiusNeverSkirts)
{
    EXPECT_FALSE(DcEngageGeometry::NeedsRoomAggroSkirt(
        -30.0f, 0.0f, 30.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(DcEngageGeometry::NeedsRoomAggroSkirt(
        -30.0f, 0.0f, 30.0f, 0.0f, 0.0f, 0.0f, -5.0f));
}
