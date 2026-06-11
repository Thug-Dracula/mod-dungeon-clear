/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

namespace
{
    DungeonBossInfo Boss(uint32 entry, uint32 idx, char const* name, uint32 mapId = 0)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.encounterIndex = idx;
        b.name = name;
        b.mapId = mapId;
        return b;
    }

    DungeonBossInfo const* Find(std::vector<DungeonBossInfo> const& v, uint32 entry)
    {
        for (DungeonBossInfo const& b : v)
            if (b.entry == entry)
                return &b;
        return nullptr;
    }
}

// --- HasPatch -------------------------------------------------------------

TEST(BossRosterRegistryTest, HasPatchOnlyForPatchedMaps)
{
    EXPECT_TRUE(BossRosterRegistry::HasPatch(189));   // SM Cathedral
    EXPECT_TRUE(BossRosterRegistry::HasPatch(109));   // Sunken Temple
    EXPECT_TRUE(BossRosterRegistry::HasPatch(209));   // ZulFarrak
    EXPECT_FALSE(BossRosterRegistry::HasPatch(0));
    EXPECT_FALSE(BossRosterRegistry::HasPatch(34));   // Stockades — no patch
}

// ZulFarrak: the summit event objective shares Chief Ukorz's bit 7 and MUST
// sort (and thus be reached) before him so the door-opening event runs first.
TEST(BossRosterRegistryTest, ZfSummitObjectiveSortsBeforeUkorz)
{
    std::vector<DungeonBossInfo> base = {
        Boss(7795, 6, "Shadowpriest Sezz'ziz", 209),
        Boss(7267, 7, "Chief Ukorz Sandscalp", 209),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(209, base);

    int objIdx = -1, ukorzIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].kind == DungeonAnchorKind::Objective)
            objIdx = i;
        if (out[i].entry == 7267)
            ukorzIdx = i;
    }
    ASSERT_GE(objIdx, 0) << "summit objective missing";
    ASSERT_GE(ukorzIdx, 0);
    EXPECT_LT(objIdx, ukorzIdx) << "objective must precede Ukorz despite equal bit";
    EXPECT_EQ(out[objIdx].encounterIndex, 7u);
}

// Sunken Temple: the summoned Avatar of Hakkar (no spawn) is represented by a
// travel objective gated on its entry (8443).
TEST(BossRosterRegistryTest, SunkenTempleAddsGatedAltarObjective)
{
    std::vector<DungeonBossInfo> base = {
        Boss(5710, 3, "Jammal'an the Prophet", 109),
        Boss(5709, 8, "Shade of Eranikus", 109),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(109, base);

    DungeonBossInfo const* obj = nullptr;
    for (DungeonBossInfo const& b : out)
        if (b.kind == DungeonAnchorKind::Objective)
            obj = &b;
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->gateEntry, 8443u);
    EXPECT_GT(obj->arriveRadius, 0.0f);
    // Slots between Jammal'an (3) and Shade of Eranikus (8).
    EXPECT_GT(obj->encounterIndex, 3u);
    EXPECT_LT(obj->encounterIndex, 8u);
}

// --- Apply: pass-through for unpatched maps -------------------------------

TEST(BossRosterRegistryTest, UnpatchedMapReturnsBaseUnchanged)
{
    std::vector<DungeonBossInfo> base = {
        Boss(1001, 0, "A"),
        Boss(1002, 1, "B"),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(34, base);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].entry, 1001u);
    EXPECT_EQ(out[1].entry, 1002u);
}

// --- Apply: SM Cathedral patch (the shipped worked example) ---------------

TEST(BossRosterRegistryTest, SmCathedralSwapsWhitemaneForMograine)
{
    // The auto-derived Cathedral list as BossSpawnIndex emits it: Fairbanks
    // (idx 4) and Whitemane (idx 5). Plus a non-Cathedral SM boss to prove the
    // patch only touches the entries it names.
    std::vector<DungeonBossInfo> base = {
        Boss(3975, 2, "Herod", 189),               // Armory — untouched
        Boss(4542, 4, "High Inquisitor Fairbanks", 189),
        Boss(3977, 5, "High Inquisitor Whitemane", 189),
    };

    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(189, base);

    // Whitemane removed.
    EXPECT_EQ(Find(out, 3977), nullptr);

    // Mograine injected as a real Boss anchor with coords.
    DungeonBossInfo const* mograine = Find(out, 3976);
    ASSERT_NE(mograine, nullptr);
    EXPECT_EQ(mograine->kind, DungeonAnchorKind::Boss);
    EXPECT_GT(mograine->x, 1100.0f);
    EXPECT_LT(mograine->x, 1200.0f);

    // He BORROWS Whitemane's encounter index (kill-bit) via inheritCompletionFrom,
    // and the authoring field is consumed (cleared) by Apply.
    EXPECT_EQ(mograine->encounterIndex, 5u);
    EXPECT_EQ(mograine->inheritCompletionFrom, 0u);

    // Untouched bosses survive.
    EXPECT_NE(Find(out, 3975), nullptr);
    EXPECT_NE(Find(out, 4542), nullptr);
}

TEST(BossRosterRegistryTest, ResultStaysEncounterOrdered)
{
    std::vector<DungeonBossInfo> base = {
        Boss(3975, 2, "Herod", 189),
        Boss(4542, 4, "High Inquisitor Fairbanks", 189),
        Boss(3977, 5, "High Inquisitor Whitemane", 189),
    };

    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(189, base);

    ASSERT_FALSE(out.empty());
    for (size_t i = 1; i < out.size(); ++i)
        EXPECT_LE(out[i - 1].encounterIndex, out[i].encounterIndex)
            << "roster not encounter-ordered at index " << i;
}

// inheritCompletionFrom must resolve against the PRE-removal base, so removing
// the source entry and inheriting from it in the same patch both work.
TEST(BossRosterRegistryTest, InheritResolvesBeforeRemoval)
{
    // Mograine inherits from 3977 while the same patch removes 3977 — the
    // inherited index must still be the one 3977 carried in `base`.
    std::vector<DungeonBossInfo> base = {
        Boss(3977, 9, "High Inquisitor Whitemane", 189),  // non-default idx
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(189, base);

    DungeonBossInfo const* mograine = Find(out, 3976);
    ASSERT_NE(mograine, nullptr);
    EXPECT_EQ(mograine->encounterIndex, 9u);
    EXPECT_EQ(Find(out, 3977), nullptr);
}
