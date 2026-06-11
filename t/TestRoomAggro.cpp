/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"

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
