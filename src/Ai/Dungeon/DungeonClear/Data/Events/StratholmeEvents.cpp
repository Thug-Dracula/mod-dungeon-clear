/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "InstanceScript.h"
#include "Player.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

// --- Stratholme (map 329) — the dead side / "Baron run" --------------------
// The Undead-side set-piece, verified from instance_stratholme.cpp. Two
// independent instance counters gate the whole thing; without orchestration the
// dungeon-clear tank pulls the open-air bosses but never finishes the gates and
// wedges. The five DBC bosses (Baroness, Nerub'enkan, Maleki, Ramstein, Baron)
// keep their natural anchors — we only add the bits the engage pipeline cannot
// reach on its own:
//
//   ZIGGURATS. Each of the three ziggurat bosses (Baroness 10436 / TYPE_ZIGGURAT1,
//   Nerub'enkan 10437 / TYPE_ZIGGURAT2, Maleki 10438 / TYPE_ZIGGURAT3) sits
//   OUTSIDE its ziggurat and is pulled normally. Its death sets TYPE_ZIGGURATx=1,
//   which OPENS the ziggurat door. INSIDE are Thuzadin Acolytes (10399); killing
//   them topples the Ash'ari Crystal (10415), whose SmartAI sets TYPE_ZIGGURATx=2
//   ("cleared"). The tank won't walk into the just-opened door for that trash on
//   its own — so a CONDITIONAL event per ziggurat fires the instant the door
//   opens (GetData==1) and ClearRadius-clears the chamber; it reads false again
//   at state 2 and never re-fires. With all three at 2 the core opens the
//   gauntlet + slaughter gates to the Slaughter Square.
//
//   SLAUGHTERHOUSE. The Slaughter Square holds pre-spawned abominations (Bile
//   Spewer 10416 / Venom Belcher 10417 — trash, not bosses). Killing them all
//   summons Ramstein the Gorger (10439); Ramstein's death spawns 33 Mindless
//   Undead (11030, wave 1); clearing those spawns 5 Black Guards (10394, wave 2,
//   ~20s later); clearing those opens the door to Baron Rivendare (10440). This
//   whole chain is ONE persistent anchored event tied to a Slaughter-Square
//   objective anchor (BossRosterRegistry, eventId 1, ordered at Ramstein's DBC
//   bit 11 so the tank arrives after the ziggurats + Barthilas and before
//   Baron). Persistent so the many separate fights (each a >1s combat tick-gap
//   that would otherwise rewind the chain) don't reset progress.
//
// IMPORTANT — no instance-data gate for the slaughter phase. The instance
// exposes only TYPE_ZIGGURAT1/2/3 via GetData(); _slaughterProgress is private.
// So unlike ZulFarrak's temple (MoveToHoldUntilInstanceData) every slaughter
// gate here is a LIVE creature / GO-state read, which survives the combat gaps
// and is observable at any time. Each WaitForSpawn carries a finite timeout and
// the following kill/clear step no-ops cleanly when nothing is alive, so a
// fight-straight-through can never wedge the run.

namespace
{
    // --- ziggurat acolyte chambers ---------------------------------------
    constexpr uint32 STR_ACOLYTE = 10399;  // Thuzadin Acolyte (inside, documentary)

    // Ziggurat boss entries — used only for panel placement: each acolyte clear
    // sorts in the `dc bosses` panel just BEFORE the NEXT anchor in clear order
    // (PanelBeforeBoss), so it renders right after the boss whose death opened
    // its door instead of being dumped at the end as a generic off-path event.
    constexpr uint32 STR_NERUBENKAN = 10437;  // after Baroness's clear
    constexpr uint32 STR_MALEKI = 10438;      // after Nerub'enkan's clear

    // Mirror of instance_stratholme.cpp DataTypes (TYPE_ZIGGURAT1/2/3). The core
    // script header isn't reachable from the module, so the values are duplicated
    // here; they are stable vanilla instance-data ids.
    constexpr uint32 STR_TYPE_ZIGGURAT1 = 1;
    constexpr uint32 STR_TYPE_ZIGGURAT2 = 2;
    constexpr uint32 STR_TYPE_ZIGGURAT3 = 3;

    // Acolyte spawn-cluster centroids per ziggurat (creature.sql, map 329). Each
    // ClearRadius is anchored INSIDE the chamber; the conditional event's engage
    // pipeline drives the tank through the now-open door to clear it.
    constexpr float STR_ZIG1_X = 3847.4f, STR_ZIG1_Y = -3749.7f, STR_ZIG1_Z = 145.2f;  // Baroness
    constexpr float STR_ZIG2_X = 3838.8f, STR_ZIG2_Y = -3498.3f, STR_ZIG2_Z = 141.5f;  // Nerub'enkan
    constexpr float STR_ZIG3_X = 4059.0f, STR_ZIG3_Y = -3668.0f, STR_ZIG3_Z = 133.0f;  // Maleki

    constexpr float STR_ZIG_RADIUS = 18.0f;   // covers the inner chamber
    constexpr float STR_ZIG_ZBAND = 12.0f;    // keep the multi-level floors out

    // --- slaughterhouse --------------------------------------------------
    constexpr uint32 STR_RAMSTEIN = 10439;     // summoned once the abominations die
    constexpr uint32 STR_MINDLESS = 11030;     // wave 1 (33x mindless undead)
    constexpr uint32 STR_BLACK_GUARD = 10394;  // wave 2 (5x black guard)

    // Slaughter Square centre — SlaughterPos from instance_stratholme.cpp, where
    // the abominations stand and Ramstein is summoned. The radius covers the
    // whole square; the zBand keeps the surrounding ramps/floors out.
    constexpr float STR_SLAUGHTER_X = 4032.20f;
    constexpr float STR_SLAUGHTER_Y = -3378.06f;
    constexpr float STR_SLAUGHTER_Z = 119.75f;
    constexpr float STR_SLAUGHTER_RADIUS = 45.0f;
    constexpr float STR_SLAUGHTER_ZBAND = 15.0f;

    constexpr uint32 STR_RAMSTEIN_SPAWN_TIMEOUT = 60000;   // 1 min — emerge delay
    constexpr uint32 STR_WAVE_TIMEOUT = 300000;            // 5 min per wave

    // DUE while a ziggurat boss is dead (door open, GetData==1) but the chamber
    // is not yet cleared (GetData==2). Reads false at 0 (boss still alive, door
    // shut) and at 2 (acolytes dead), so the event fires exactly in the
    // "acolytes still up" window and never re-fires — the instance state is the
    // latch, no ConditionalLatchKey needed.
    bool ZigguratAcolytes(Player* bot, uint32 dataType)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        return inst && inst->GetData(dataType) == 1;
    }

    bool StrZig1(Player* bot, AiObjectContext* /*context*/)
    {
        return ZigguratAcolytes(bot, STR_TYPE_ZIGGURAT1);
    }

    bool StrZig2(Player* bot, AiObjectContext* /*context*/)
    {
        return ZigguratAcolytes(bot, STR_TYPE_ZIGGURAT2);
    }

    bool StrZig3(Player* bot, AiObjectContext* /*context*/)
    {
        return ZigguratAcolytes(bot, STR_TYPE_ZIGGURAT3);
    }
}

void RegisterStratholmeEvents(std::vector<DungeonEvent>& out)
{
    // --- three ziggurat acolyte clears (conditional, fire on door-open) ----
    // ClearRadius (position-based) rather than KillCreature(10399): it drives
    // the tank into the chamber and clears whatever guards the crystal without
    // depending on the exact acolyte count, and the zBand keeps the chamber's
    // adjacent levels out of the clear. (void STR_ACOLYTE — documentary entry.)
    (void) STR_ACOLYTE;

    out.push_back(EventBuilder(329, 1, "Ziggurat 1 acolytes (Baroness)")
                      .Conditional(5)
                      .PanelBeforeBoss(STR_NERUBENKAN)
                      .ClearRadius(STR_ZIG1_X, STR_ZIG1_Y, STR_ZIG1_Z,
                                   STR_ZIG_RADIUS, STR_ZIG_ZBAND)
                      .Build());

    out.push_back(EventBuilder(329, 2, "Ziggurat 2 acolytes (Nerub'enkan)")
                      .Conditional(6)
                      .PanelBeforeBoss(STR_MALEKI)
                      .ClearRadius(STR_ZIG2_X, STR_ZIG2_Y, STR_ZIG2_Z,
                                   STR_ZIG_RADIUS, STR_ZIG_ZBAND)
                      .Build());

    out.push_back(EventBuilder(329, 3, "Ziggurat 3 acolytes (Maleki)")
                      .Conditional(7)
                      // After Maleki's clear the next anchor is the Slaughterhouse
                      // objective; sort just before it (all three ziggurats must be
                      // cleared before that gate opens).
                      .PanelBeforeBoss(BossRosterRegistry::ObjectiveEntry(1))
                      .ClearRadius(STR_ZIG3_X, STR_ZIG3_Y, STR_ZIG3_Z,
                                   STR_ZIG_RADIUS, STR_ZIG_ZBAND)
                      .Build());

    // --- the slaughterhouse chain (persistent anchored, eventId 1) ---------
    // Anchored at the Slaughter-Square objective (BossRosterRegistry, OBJ(1),
    // encounterIndex 11). Boss-nav travels the tank in; the steps then run the
    // abomination -> Ramstein -> wave1 -> wave2 chain. Persistent so the kills
    // (each a >1s combat tick-gap) don't rewind the step list.
    out.push_back(EventBuilder(329, 4, "Slaughterhouse (Baron run)")
                      .Anchored(11)
                      .Persistent()
                      // 1. Clear the pre-spawned abominations -> summons Ramstein.
                      //    ClearRadius engages every hostile in the square (both
                      //    abomination entries) without a fixed count.
                      .ClearRadius(STR_SLAUGHTER_X, STR_SLAUGHTER_Y, STR_SLAUGHTER_Z,
                                   STR_SLAUGHTER_RADIUS, STR_SLAUGHTER_ZBAND)
                      // 2. Wait for Ramstein to materialise, then seek + kill him.
                      //    WaitForSpawn is essential or KillCreatureEngage would
                      //    read "no live Ramstein" before the summon and complete.
                      .WaitForSpawn(STR_RAMSTEIN, /*alive*/ true)
                          .Timeout(STR_RAMSTEIN_SPAWN_TIMEOUT)
                      .KillCreatureEngage(STR_RAMSTEIN, /*count*/ 1, /*searchRadius*/ 100.0f)
                      // 3. Wave 1: 33 Mindless Undead spawn over ~7s. Wait for the
                      //    first, then ClearRadius until the square is empty
                      //    (count-tolerant; they are TEMPSUMMON and despawn dead).
                      .WaitForSpawn(STR_MINDLESS, /*alive*/ true)
                          .Timeout(STR_WAVE_TIMEOUT)
                      .ClearRadius(STR_SLAUGHTER_X, STR_SLAUGHTER_Y, STR_SLAUGHTER_Z,
                                   STR_SLAUGHTER_RADIUS, STR_SLAUGHTER_ZBAND)
                      // 4. Wave 2: 5 Black Guards spawn ~20s after wave 1 clears.
                      //    Their death opens the door to Baron, who is then a
                      //    normal boss anchor (DBC bit 12) reached next.
                      .WaitForSpawn(STR_BLACK_GUARD, /*alive*/ true)
                          .Timeout(STR_WAVE_TIMEOUT)
                      .KillCreature(STR_BLACK_GUARD, /*count*/ 1, /*searchRadius*/ 60.0f)
                      .Build());
}

void RegisterStratholmeConditions(EventConditionMap& out)
{
    out[5] = &StrZig1;
    out[6] = &StrZig2;
    out[7] = &StrZig3;
}
