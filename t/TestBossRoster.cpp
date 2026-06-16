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
    EXPECT_TRUE(BossRosterRegistry::HasPatch(230));   // Blackrock Depths
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

// Blackrock Depths: the Ring of Law objective (its own DungeonEncounter bit 3,
// credited to the spawn-less Grimstone) is injected between Houndmaster Grebmar
// (bit 2) and Pyromancer Loregrain (bit 4), carrying the arena event.
TEST(BossRosterRegistryTest, RingOfLawObjectiveSortsBetweenGrebmarAndLoregrain)
{
    std::vector<DungeonBossInfo> base = {
        Boss(9319, 2, "Houndmaster Grebmar", 230),
        Boss(9024, 4, "Pyromancer Loregrain", 230),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(230, base);

    int grebmarIdx = -1, ringIdx = -1, loregrainIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].entry == 9319)
            grebmarIdx = i;
        if (out[i].kind == DungeonAnchorKind::Objective)
            ringIdx = i;
        if (out[i].entry == 9024)
            loregrainIdx = i;
    }
    ASSERT_GE(ringIdx, 0) << "Ring of Law objective missing";
    ASSERT_GE(grebmarIdx, 0);
    ASSERT_GE(loregrainIdx, 0);
    EXPECT_LT(grebmarIdx, ringIdx) << "Ring of Law must follow Grebmar";
    EXPECT_LT(ringIdx, loregrainIdx) << "Ring of Law must precede Loregrain";
    EXPECT_EQ(out[ringIdx].encounterIndex, 3u);
    EXPECT_EQ(out[ringIdx].eventId, 1u);
}

// Sunken Temple: the DBC bit order is NOT a valid clear order. The roster removes
// the three phase/puzzle-gated bosses (Weaver 5720, Dreamscythe 5721, Atal'alarion
// 8580) from their low bits and re-adds them — plus the statue/idol/Avatar pit
// wing — as event-bearing objective anchors. Weaver & Dreamscythe land on the
// required spine (after Jammal'an, before Eranikus); the whole pit wing lands at
// the route tail (after Eranikus).
TEST(BossRosterRegistryTest, SunkenTempleReordersPhaseGatedBosses)
{
    std::vector<DungeonBossInfo> base = {
        Boss(8580, 0, "Atal'alarion", 109),
        Boss(5721, 1, "Dreamscythe", 109),
        Boss(5720, 2, "Weaver", 109),
        Boss(5710, 3, "Jammal'an the Prophet", 109),
        Boss(5719, 4, "Morphaz", 109),
        Boss(5722, 6, "Hazzas", 109),
        Boss(5709, 8, "Shade of Eranikus", 109),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(109, base);

    // The three phase/puzzle-gated bosses are gone as combat bosses.
    EXPECT_EQ(Find(out, 8580), nullptr);
    EXPECT_EQ(Find(out, 5721), nullptr);
    EXPECT_EQ(Find(out, 5720), nullptr);

    // Kept auto bosses survive with their real bits.
    ASSERT_NE(Find(out, 5710), nullptr);
    EXPECT_EQ(Find(out, 5710)->encounterIndex, 3u);
    ASSERT_NE(Find(out, 5709), nullptr);

    auto pos = [&](uint32 entry)
    {
        for (int i = 0; i < (int)out.size(); ++i)
            if (out[i].entry == entry)
                return i;
        return -1;
    };
    // Locate the re-added objectives by their event ids: a forcefield ring anchor
    // (1), Weaver & Dreamscythe (10), statue 1 (2), and the Avatar (9).
    int ffPos = -1, wdPos = -1, statuePos = -1, avatarPos = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].kind != DungeonAnchorKind::Objective)
            continue;
        if (out[i].eventId == 1)
            ffPos = i;
        if (out[i].eventId == 10)
            wdPos = i;
        if (out[i].eventId == 2)
            statuePos = i;
        if (out[i].eventId == 9)
            avatarPos = i;
    }
    ASSERT_GE(ffPos, 0) << "forcefield ring anchor missing";
    ASSERT_GE(wdPos, 0) << "Weaver & Dreamscythe objective missing";
    ASSERT_GE(statuePos, 0) << "statue objective missing";
    ASSERT_GE(avatarPos, 0) << "Avatar objective missing";

    // Forcefield ring anchors come first, before Jammal'an (the gate to him).
    EXPECT_LT(ffPos, pos(5710));

    // Required spine: Weaver & Dreamscythe after Jammal'an, before Eranikus.
    EXPECT_LT(pos(5710), wdPos);
    EXPECT_LT(wdPos, pos(5709));

    // Optional pit wing (statues..Avatar) at the tail, after Eranikus.
    EXPECT_LT(pos(5709), statuePos);
    EXPECT_LT(statuePos, avatarPos);
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
