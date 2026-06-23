/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
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
    EXPECT_TRUE(BossRosterRegistry::HasPatch(36));    // Deadmines
    EXPECT_TRUE(BossRosterRegistry::HasPatch(329));   // Stratholme
    EXPECT_TRUE(BossRosterRegistry::HasPatch(289));   // Scholomance
    EXPECT_TRUE(BossRosterRegistry::HasPatch(429));   // Dire Maul East
    EXPECT_TRUE(BossRosterRegistry::HasPatch(70));    // Uldaman — altar objectives
    EXPECT_FALSE(BossRosterRegistry::HasPatch(0));
    EXPECT_FALSE(BossRosterRegistry::HasPatch(34));   // Stockades — no patch
}

// Uldaman: the two altar travel objectives MUST sort between Grimlok (the boss
// before them) and Archaedas (the final boss, reordered above them), in the
// order keeper-altar -> Archaedas-altar -> Archaedas. The keeper altar opens the
// temple door; the Archaedas altar wakes the stoned boss; both must precede him.
TEST(BossRosterRegistryTest, UldamanAltarObjectivesSortBeforeArchaedas)
{
    std::vector<DungeonBossInfo> base = {
        Boss(4854, 6, "Grimlok", 70),
        Boss(2748, 7, "Archaedas", 70),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(70, base);

    int grimlokIdx = -1, keeperIdx = -1, archAltarIdx = -1, archaedasIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].entry == 4854)
            grimlokIdx = i;
        else if (out[i].entry == 2748)
            archaedasIdx = i;
        else if (out[i].kind == DungeonAnchorKind::Objective && out[i].eventId == 2)
            keeperIdx = i;
        else if (out[i].kind == DungeonAnchorKind::Objective && out[i].eventId == 3)
            archAltarIdx = i;
    }

    ASSERT_NE(grimlokIdx, -1);
    ASSERT_NE(keeperIdx, -1);
    ASSERT_NE(archAltarIdx, -1);
    ASSERT_NE(archaedasIdx, -1);
    EXPECT_LT(grimlokIdx, keeperIdx);
    EXPECT_LT(keeperIdx, archAltarIdx);
    EXPECT_LT(archAltarIdx, archaedasIdx);

    // Archaedas keeps his real DBC kill-bit (7) — only his clear ORDER moved.
    EXPECT_EQ(out[archaedasIdx].encounterIndex, 7u);
}

// ZulFarrak: the Temple Summit event objective (orderOverride 4) MUST sort (and
// thus be reached) before Chief Ukorz (orderOverride 5) so the door-opening
// event runs first.
TEST(BossRosterRegistryTest, ZfSummitObjectiveSortsBeforeUkorz)
{
    std::vector<DungeonBossInfo> base = {
        Boss(7795, 0, "Hydromancer Velratha", 209),
        Boss(7267, 7, "Chief Ukorz Sandscalp", 209),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(209, base);

    int objIdx = -1, ukorzIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        // The summit objective shares bit 7 with Ukorz; the Gahz'rilla objective
        // sits at bit 8, so key off the index to pick the summit one.
        if (out[i].kind == DungeonAnchorKind::Objective && out[i].encounterIndex == 7u)
            objIdx = i;
        if (out[i].entry == 7267)
            ukorzIdx = i;
    }
    ASSERT_GE(objIdx, 0) << "summit objective missing";
    ASSERT_GE(ukorzIdx, 0);
    EXPECT_LT(objIdx, ukorzIdx) << "objective must precede Ukorz despite equal bit";
    EXPECT_EQ(out[objIdx].encounterIndex, 7u);
}

// ZulFarrak: the optional Gahz'rilla gong objective (orderOverride 7) MUST sort
// AFTER Chief Ukorz (orderOverride 5) AND after Hydromancer Velratha (6) so the
// strictly-ordinal picker only routes the tank to the sacred pool once every
// real boss is dead — the "very last" stop.
TEST(BossRosterRegistryTest, ZfGahzrillaObjectiveSortsAfterUkorz)
{
    std::vector<DungeonBossInfo> base = {
        Boss(7795, 0, "Hydromancer Velratha", 209),
        Boss(7267, 7, "Chief Ukorz Sandscalp", 209),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(209, base);

    int gahzIdx = -1, ukorzIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].kind == DungeonAnchorKind::Objective && out[i].encounterIndex == 8u)
            gahzIdx = i;
        if (out[i].entry == 7267)
            ukorzIdx = i;
    }
    ASSERT_GE(gahzIdx, 0) << "Gahz'rilla objective missing";
    ASSERT_GE(ukorzIdx, 0);
    EXPECT_GT(gahzIdx, ukorzIdx) << "Gahz'rilla must come after Ukorz (ordered last)";
    EXPECT_EQ(out[gahzIdx].eventId, 2u);
}

// ZulFarrak FULL clear order: the DBC bits (Velratha 0, Antu'sul 2, Theka 3,
// Zum'rah 4, Ukorz 7) do not match the travel path. The `reorder` patch stamps
// orderOverride 1..6 on the five real bosses (kill-bits untouched) and slots the
// two objectives in at 4 and 7, yielding:
//   Theka -> Antu'sul -> Zum'rah -> Temple Summit -> Ukorz -> Velratha -> Pool.
TEST(BossRosterRegistryTest, ZfFullClearOrder)
{
    // Auto-derived list as BossSpawnIndex emits it (DBC encounterIndex order).
    std::vector<DungeonBossInfo> base = {
        Boss(7795, 0, "Hydromancer Velratha", 209),
        Boss(8127, 2, "Antu'sul", 209),
        Boss(7272, 3, "Theka the Martyr", 209),
        Boss(7271, 4, "Witch Doctor Zum'rah", 209),
        Boss(7267, 7, "Chief Ukorz Sandscalp", 209),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(209, base);

    auto pos = [&](uint32 entry)
    {
        for (int i = 0; i < (int)out.size(); ++i)
            if (out[i].entry == entry)
                return i;
        return -1;
    };
    int summitPos = -1, poolPos = -1;
    for (int i = 0; i < (int)out.size(); ++i)
        if (out[i].kind == DungeonAnchorKind::Objective)
        {
            if (out[i].eventId == 1u) summitPos = i;
            if (out[i].eventId == 2u) poolPos = i;
        }
    ASSERT_GE(summitPos, 0) << "Temple Summit objective missing";
    ASSERT_GE(poolPos, 0) << "Sacred Pool objective missing";

    // Theka -> Antu'sul -> Zum'rah -> Temple Summit -> Ukorz -> Velratha -> Pool.
    EXPECT_LT(pos(7272), pos(8127));
    EXPECT_LT(pos(8127), pos(7271));
    EXPECT_LT(pos(7271), summitPos);
    EXPECT_LT(summitPos, pos(7267));
    EXPECT_LT(pos(7267), pos(7795));
    EXPECT_LT(pos(7795), poolPos);

    // Reordering must NOT disturb the real DBC kill-bits.
    EXPECT_EQ(Find(out, 7795)->encounterIndex, 0u);  // Velratha
    EXPECT_EQ(Find(out, 7267)->encounterIndex, 7u);  // Ukorz
    EXPECT_EQ(BossOrderKey(*Find(out, 7795)), 6u);
    EXPECT_EQ(BossOrderKey(*Find(out, 7267)), 5u);
}

// Stratholme dead side: the DBC puts the ziggurats (Baroness 7, Nerub'enkan 8,
// Maleki 9) before Magistrate Barthilas (10), but the path runs Barthilas FIRST.
// The patch re-adds Barthilas with orderOverride 6 (keeping kill-bit 10) so the
// clear order becomes Barthilas -> ziggurats -> Slaughterhouse (11) -> Baron (12).
//
// The live side here is just Archivist Galford (bit 5). Balnazzar (bit 6) is NOT
// auto-derived in production: its credit creature 10813 has no creature.sql spawn
// (it exists only via Dathrohan's UpdateEntry), so BossSpawnIndex drops it — the
// base list jumps Galford(5) -> Baroness(7). The patch fills the gap with a
// Dathrohan objective (OBJ(2), eventId 5, bit 6).
TEST(BossRosterRegistryTest, StratholmeBarthilasReorderedBeforeZiggurats)
{
    std::vector<DungeonBossInfo> base = {
        Boss(10811, 5, "Archivist Galford", 329),  // live side, last auto-derived
        Boss(10436, 7, "Baroness Anastari", 329),
        Boss(10437, 8, "Nerub'enkan", 329),
        Boss(10438, 9, "Maleki the Pallid", 329),
        Boss(10435, 10, "Magistrate Barthilas", 329),
        Boss(10440, 12, "Baron Rivendare", 329),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(329, base);

    int galfordIdx = -1, dathrohanIdx = -1, barthIdx = -1, baronessIdx = -1,
        malekiIdx = -1, slaughterIdx = -1, baronIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].entry == 10811) galfordIdx = i;
        if (out[i].kind == DungeonAnchorKind::Objective && out[i].eventId == 5u) dathrohanIdx = i;
        if (out[i].entry == 10435) barthIdx = i;
        if (out[i].entry == 10436) baronessIdx = i;
        if (out[i].entry == 10438) malekiIdx = i;
        if (out[i].kind == DungeonAnchorKind::Objective && out[i].eventId == 4u) slaughterIdx = i;
        if (out[i].entry == 10440) baronIdx = i;
    }
    ASSERT_GE(galfordIdx, 0);
    ASSERT_GE(dathrohanIdx, 0) << "Dathrohan (Balnazzar) objective missing";
    ASSERT_GE(barthIdx, 0);
    ASSERT_GE(baronessIdx, 0);
    ASSERT_GE(slaughterIdx, 0) << "slaughterhouse objective missing";
    ASSERT_GE(baronIdx, 0);

    // Galford -> Dathrohan/Balnazzar -> Barthilas -> ziggurats -> slaughter -> Baron.
    EXPECT_LT(galfordIdx, dathrohanIdx) << "Dathrohan follows Galford on the live side";
    EXPECT_LT(dathrohanIdx, barthIdx) << "live side (Balnazzar) before the dead side";
    EXPECT_LT(barthIdx, baronessIdx) << "Barthilas must precede the ziggurats";
    EXPECT_LT(malekiIdx, slaughterIdx) << "ziggurats before the slaughterhouse";
    EXPECT_LT(slaughterIdx, baronIdx) << "slaughterhouse before Baron";

    // The Dathrohan objective slots at bit 6 (after Galford 5) and is an Objective
    // (no real kill-bit — completion is its event, not the mask).
    EXPECT_EQ(out[dathrohanIdx].kind, DungeonAnchorKind::Objective);
    EXPECT_EQ(BossOrderKey(out[dathrohanIdx]), 6u);

    // Reordering must NOT disturb Barthilas's real kill-bit: completion still keys on 10.
    EXPECT_EQ(out[barthIdx].encounterIndex, 10u);
    EXPECT_EQ(out[barthIdx].orderOverride, 6);
    EXPECT_EQ(BossOrderKey(out[barthIdx]), 6u);
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

// Deadmines: the Defias Cannon objective shares Mr. Smite's bit (3); the
// objective-before-boss tie-break must order it after Gilnid (bit 2) and before
// Mr. Smite, so the tank opens the Iron Clad Door before heading to the ship.
TEST(BossRosterRegistryTest, IronCladDoorSortsBetweenGilnidAndMrSmite)
{
    std::vector<DungeonBossInfo> base = {
        Boss(1763, 2, "Gilnid", 36),
        Boss(646, 3, "Mr. Smite", 36),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(36, base);

    int gilnidIdx = -1, doorIdx = -1, smiteIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].entry == 1763)
            gilnidIdx = i;
        if (out[i].kind == DungeonAnchorKind::Objective)
            doorIdx = i;
        if (out[i].entry == 646)
            smiteIdx = i;
    }
    ASSERT_GE(doorIdx, 0) << "Iron Clad Door objective missing";
    ASSERT_GE(gilnidIdx, 0);
    ASSERT_GE(smiteIdx, 0);
    EXPECT_LT(gilnidIdx, doorIdx) << "cannon must follow Gilnid";
    EXPECT_LT(doorIdx, smiteIdx) << "cannon must precede Mr. Smite";
    EXPECT_EQ(out[doorIdx].encounterIndex, 3u);
    EXPECT_EQ(out[doorIdx].eventId, 1u);
}

// Dire Maul East: the Ironbark / Conservatory Door objective (orderOverride 40,
// eventId 1) must sort after the three southern bosses (reordered 10/20/30) and
// before Alzzin the Wildshaper (reordered 50), so the tank gossips Ironbark to
// open the door before heading to Alzzin's grove. The reorder leaves the real
// DBC kill-bits (the base encounterIndex) untouched.
TEST(BossRosterRegistryTest, DireMaulEastIronbarkSortsBeforeAlzzin)
{
    std::vector<DungeonBossInfo> base = {
        Boss(11490, 0, "Zevrim Thornhoof", 429),
        Boss(13280, 1, "Hydrospawn", 429),
        Boss(14327, 2, "Lethtendris", 429),
        Boss(11492, 3, "Alzzin the Wildshaper", 429),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(429, base);

    // NOTE: Dire Maul shares ONE map-429 patch across wings, so Apply() also
    // appends the West pylon objectives (eventId 4-8). Identify Ironbark by his
    // eventId (1), not "any objective", which would now match a West pylon.
    int lethIdx = -1, ironbarkIdx = -1, alzzinIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].entry == 14327)
            lethIdx = i;
        if (out[i].kind == DungeonAnchorKind::Objective && out[i].eventId == 1)
            ironbarkIdx = i;
        if (out[i].entry == 11492)
            alzzinIdx = i;
    }
    ASSERT_GE(ironbarkIdx, 0) << "Ironbark / Conservatory Door objective missing";
    ASSERT_GE(lethIdx, 0);
    ASSERT_GE(alzzinIdx, 0);
    EXPECT_LT(lethIdx, ironbarkIdx) << "Ironbark must follow the southern bosses";
    EXPECT_LT(ironbarkIdx, alzzinIdx) << "Ironbark must precede Alzzin";
    EXPECT_EQ(out[ironbarkIdx].eventId, 1u);

    // Reorder keys: 10/20/30/objective 40/50; real kill-bits untouched.
    EXPECT_EQ(BossOrderKey(*Find(out, 14327)), 30u);
    EXPECT_EQ(BossOrderKey(out[ironbarkIdx]), 40u);
    EXPECT_EQ(BossOrderKey(*Find(out, 11492)), 50u);
    EXPECT_EQ(Find(out, 11492)->encounterIndex, 3u) << "Alzzin DBC kill-bit intact";
}

// Dire Maul West: the kill order is fixed (Immol'thar before the friendly-until-
// his-death Prince), and Immol'thar's five Crystal Generator pylons are added as
// travel objectives — three before Tendris, two before Immol'thar. Real DBC
// kill-bits stay intact; objective encounterIndex values are synthetic highs.
TEST(BossRosterRegistryTest, DireMaulWestPylonsAndOrder)
{
    // Arbitrary DBC bit order (deliberately NOT the kill order) to prove the
    // reorder is what fixes it.
    std::vector<DungeonBossInfo> base = {
        Boss(11486, 0, "Prince Tortheldrin", 429),
        Boss(11496, 1, "Immol'thar", 429),
        Boss(11487, 2, "Magister Kalendris", 429),
        Boss(11488, 3, "Illyanna Ravenoak", 429),
        Boss(11489, 4, "Tendris Warpwood", 429),
    };
    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(429, base);

    // All five West bosses survive with their real kill-bits untouched.
    ASSERT_NE(Find(out, 11489), nullptr);
    EXPECT_EQ(Find(out, 11489)->encounterIndex, 4u) << "Tendris DBC kill-bit intact";
    EXPECT_EQ(Find(out, 11486)->encounterIndex, 0u) << "Prince DBC kill-bit intact";

    // Reorder keys put the kill order right: Tendris < Illyanna < Kalendris <
    // Immol'thar < Prince.
    EXPECT_EQ(BossOrderKey(*Find(out, 11489)), 10u);
    EXPECT_EQ(BossOrderKey(*Find(out, 11488)), 20u);
    EXPECT_EQ(BossOrderKey(*Find(out, 11487)), 30u);
    EXPECT_EQ(BossOrderKey(*Find(out, 11496)), 45u);
    EXPECT_EQ(BossOrderKey(*Find(out, 11486)), 50u);

    // Immol'thar strictly precedes the Prince in the final order.
    int immolIdx = -1, princeIdx = -1;
    for (int i = 0; i < (int)out.size(); ++i)
    {
        if (out[i].entry == 11496)
            immolIdx = i;
        if (out[i].entry == 11486)
            princeIdx = i;
    }
    ASSERT_GE(immolIdx, 0);
    ASSERT_GE(princeIdx, 0);
    EXPECT_LT(immolIdx, princeIdx) << "Immol'thar must die before the Prince";

    // The five pylon objectives are present, wired to events 4-8, with synthetic
    // (>= 32, so never confused with a real DBC bit) encounterIndex values.
    int pylonCount = 0;
    for (DungeonBossInfo const& b : out)
    {
        if (b.kind == DungeonAnchorKind::Objective && b.eventId >= 4 && b.eventId <= 8)
        {
            ++pylonCount;
            EXPECT_GE(b.encounterIndex, 32u)
                << "pylon objective kill-bit must be synthetic-high";
        }
    }
    EXPECT_EQ(pylonCount, 5) << "all five Crystal Generator objectives present";

    // Two pylons are placed after Kalendris (order 40/41), three before Tendris
    // (order 5/6/7) — i.e. the northern pair comes after Immol'thar's order in
    // the list? No: orderOverride 40/41 < Immol'thar's 45, so both northern
    // pylons precede the Immol'thar engage.
    EXPECT_LT(BossOrderKey(*Find(out, BossRosterRegistry::ObjectiveEntry(5))), 45u);
    EXPECT_LT(BossOrderKey(*Find(out, BossRosterRegistry::ObjectiveEntry(6))), 45u);
    EXPECT_LT(BossOrderKey(*Find(out, BossRosterRegistry::ObjectiveEntry(2))),
              BossOrderKey(*Find(out, 11489)))
        << "southern pylon trio routed before Tendris";

    // Barrier-skirt waypoint (OBJ 7) is sequenced strictly between the two
    // northern pylons (Gen4 OBJ5 -> waypoint -> Gen5 OBJ6) so the route around
    // Immol'thar's force field is taken between them, and it carries no event.
    DungeonBossInfo const* wp = Find(out, BossRosterRegistry::ObjectiveEntry(7));
    ASSERT_NE(wp, nullptr);
    EXPECT_EQ(wp->kind, DungeonAnchorKind::Objective);
    EXPECT_EQ(wp->eventId, 0u) << "waypoint is a pure travel anchor (no event)";
    EXPECT_LT(BossOrderKey(*Find(out, BossRosterRegistry::ObjectiveEntry(5))),
              BossOrderKey(*wp));
    EXPECT_LT(BossOrderKey(*wp),
              BossOrderKey(*Find(out, BossRosterRegistry::ObjectiveEntry(6))));

    // The Warpwood entrance sweep (OBJ 8 west / OBJ 9 east) exists and is ordered
    // FIRST — before every pylon and Tendris — so the entrance room is swept on
    // the way in.
    for (uint32 obj : {8u, 9u})
    {
        DungeonBossInfo const* sweep = Find(out, BossRosterRegistry::ObjectiveEntry(obj));
        ASSERT_NE(sweep, nullptr);
        EXPECT_LT(BossOrderKey(*sweep),
                  BossOrderKey(*Find(out, BossRosterRegistry::ObjectiveEntry(2))))
            << "entrance sweep OBJ" << obj << " precedes crystal generator 1";
        EXPECT_LT(BossOrderKey(*sweep), BossOrderKey(*Find(out, 11489)));
    }

    // REGRESSION GUARD: every objective that carries a ClearRadius (the entrance
    // pre-clear AND the five crystals) must have a MODERATE arriveRadius. Two ways
    // it has broken live, both guarded here:
    //   * too SMALL (< the ~10-17yd the tank parks short of an off-mesh GO dais)
    //     -> the tank never "arrives", thrashes in travel (deadlock). Need >= 25.
    //   * too LARGE -> the tank "arrives" far from the mobs, the ClearRadius gate
    //     finds nothing loaded and completes in 0ms (premature no-op). Keep it
    //     within ~ClearRadius + 20 so "arrived" means "among the mobs".
    for (uint32 obj : {8u, 9u, 2u, 3u, 4u, 5u, 6u})
    {
        DungeonBossInfo const* o = Find(out, BossRosterRegistry::ObjectiveEntry(obj));
        ASSERT_NE(o, nullptr);
        DungeonEvent const* e = DungeonEventRegistry::Find(429, o->eventId);
        ASSERT_NE(e, nullptr);
        for (auto const& step : e->steps)
            if (step.kind == EventStepKind::ClearRadius)
            {
                EXPECT_GE(o->arriveRadius, 25.0f)
                    << "OBJ" << obj << " arriveRadius too small -> deadlock risk";
                EXPECT_LE(o->arriveRadius, step.radius + 20.0f)
                    << "OBJ" << obj << " arriveRadius too large -> premature 0ms clear";
            }
    }
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

    // ORDER FIX: room-clear + Mograine run BEFORE Fairbanks. Mograine carries
    // orderOverride 3 (< Fairbanks's bit 4) so the picker reaches him first,
    // while his completion still keys on Whitemane's real bit 5.
    EXPECT_EQ(mograine->orderOverride, 3);
    EXPECT_EQ(BossOrderKey(*mograine), 3u);

    DungeonBossInfo const* fairbanks = Find(out, 4542);
    ASSERT_NE(fairbanks, nullptr);
    // Result is ordered by BossOrderKey, so Mograine precedes Fairbanks.
    auto pos = [&](uint32 entry)
    {
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i].entry == entry)
                return (int)i;
        return -1;
    };
    EXPECT_LT(pos(3976), pos(4542));

    // Untouched bosses survive.
    EXPECT_NE(Find(out, 3975), nullptr);
    EXPECT_NE(Find(out, 4542), nullptr);
}

// --- Apply: Scholomance merges Marduk & Vectus into one boss --------------

TEST(BossRosterRegistryTest, ScholomanceMergesMardukAndVectus)
{
    // The auto-derived list carries Vectus (10432) and Marduk (10433) as two
    // separate encounters. Plus an untouched Scholomance boss to prove the patch
    // only touches the entries it names.
    std::vector<DungeonBossInfo> base = {
        Boss(10506, 0, "Kirtonos the Herald", 289),  // untouched
        Boss(10432, 3, "Vectus", 289),
        Boss(10433, 4, "Marduk Blackpool", 289),
    };

    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(289, base);

    // Both originals collapse: Marduk is gone entirely and Vectus's entry is
    // re-added as the single merged anchor.
    EXPECT_EQ(Find(out, 10433), nullptr);

    DungeonBossInfo const* merged = Find(out, 10432);
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(merged->name, "Marduk & Vectus");
    // Anchored on Vectus's spawn (the boss nearest the close room trash).
    EXPECT_GT(merged->x, 140.0f);
    EXPECT_LT(merged->x, 150.0f);
    // Inherits Vectus's own kill-bit; the authoring field is consumed.
    EXPECT_EQ(merged->encounterIndex, 3u);
    EXPECT_EQ(merged->inheritCompletionFrom, 0u);

    // Exactly one anchor remains for the pair (no duplicate 10432).
    int merged10432 = 0;
    for (DungeonBossInfo const& b : out)
        if (b.entry == 10432)
            ++merged10432;
    EXPECT_EQ(merged10432, 1);

    EXPECT_NE(Find(out, 10506), nullptr);
}

TEST(BossRosterRegistryTest, ResultStaysClearOrdered)
{
    std::vector<DungeonBossInfo> base = {
        Boss(3975, 2, "Herod", 189),
        Boss(4542, 4, "High Inquisitor Fairbanks", 189),
        Boss(3977, 5, "High Inquisitor Whitemane", 189),
    };

    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(189, base);

    // The result is sorted by clear ORDER key (orderOverride when set, else
    // encounterIndex), NOT by raw encounterIndex — Mograine's orderOverride 3
    // deliberately places him (kill-bit 5) ahead of Fairbanks (bit 4).
    ASSERT_FALSE(out.empty());
    for (size_t i = 1; i < out.size(); ++i)
        EXPECT_LE(BossOrderKey(out[i - 1]), BossOrderKey(out[i]))
            << "roster not clear-ordered at index " << i;
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

// Wailing Caverns: the Mutanus finale. The Disciple/escort OBJECTIVE (eventId 2)
// and the summoned Mutanus BOSS both key at 7 (after Verdan's bit 6); the
// objective-before-boss tie-break puts the escort FIRST, then Mutanus. Mutanus is
// a TempSummon BossSpawnIndex can't emit, so he is added with his real
// DungeonEncounter bit 7 (instance_encounters credit 3654) so his kill completes
// the dungeon.
TEST(BossRosterRegistryTest, WailingCavernsEscortObjectiveSortsBeforeMutanus)
{
    // Auto-derived static bosses (DBC encounterIndex order), bits 0-6.
    std::vector<DungeonBossInfo> base = {
        Boss(3671, 0, "Lady Anacondra", 43),
        Boss(3669, 1, "Lord Cobrahn", 43),
        Boss(3653, 2, "Kresh", 43),
        Boss(3670, 3, "Lord Pythas", 43),
        Boss(3674, 4, "Skum", 43),
        Boss(3673, 5, "Lord Serpentis", 43),
        Boss(5775, 6, "Verdan the Everliving", 43),
    };

    std::vector<DungeonBossInfo> out = BossRosterRegistry::Apply(43, base);

    DungeonBossInfo const* escort = nullptr;
    DungeonBossInfo const* drop = nullptr;
    DungeonBossInfo const* mutanus = Find(out, 3654);
    int escortIdx = -1, dropIdx = -1, mutanusIdx = -1, verdanIdx = -1;
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i].kind == DungeonAnchorKind::Objective && out[i].eventId == 2)
        {
            escort = &out[i];
            escortIdx = static_cast<int>(i);
        }
        if (out[i].kind == DungeonAnchorKind::Objective && out[i].eventId == 3)
        {
            drop = &out[i];
            dropIdx = static_cast<int>(i);
        }
        if (out[i].entry == 3654) mutanusIdx = static_cast<int>(i);
        if (out[i].entry == 5775) verdanIdx = static_cast<int>(i);
    }

    ASSERT_NE(escort, nullptr);
    ASSERT_NE(drop, nullptr);
    ASSERT_NE(mutanus, nullptr);

    // Mutanus carries his real kill-bit 7 and is a combat boss.
    EXPECT_EQ(mutanus->encounterIndex, 7u);
    EXPECT_EQ(mutanus->kind, DungeonAnchorKind::Boss);

    // The escort objective wires event 2 and arrives generously (among the
    // Disciple, who the gossip then closes the rest of the way to).
    EXPECT_EQ(escort->encounterIndex, 7u);
    EXPECT_FLOAT_EQ(escort->arriveRadius, 18.0f);

    // The return-fall objective wires event 3 and also keys at 7 (an objective's
    // index is an ordering hint; it latches by entry, so sharing Mutanus's bit is
    // harmless). It sorts BEFORE the escort by insertion order (stable_sort).
    EXPECT_EQ(drop->encounterIndex, 7u);
    EXPECT_EQ(drop->kind, DungeonAnchorKind::Objective);

    // Order: Verdan (6) -> hole-drop (7, obj) -> escort (7, obj) -> Mutanus (7, boss).
    EXPECT_LT(verdanIdx, dropIdx);
    EXPECT_LT(dropIdx, escortIdx);
    EXPECT_LT(escortIdx, mutanusIdx);
}
