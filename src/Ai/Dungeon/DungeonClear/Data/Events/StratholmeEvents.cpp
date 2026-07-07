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

// --- Stratholme (map 329) — live (Scarlet) side: Dathrohan -> Balnazzar -----
// The Balnazzar encounter (DBC bit 6) credits creature 10813 (Balnazzar), which
// has NO creature.sql spawn: it exists only via Grand Crusader Dathrohan's
// (10812) SmartAI, which at <=40% HP casts Balnazzar Transform and UpdateEntry's
// the SAME creature in place (10812 -> 10813, GUID preserved). BossSpawnIndex
// matches credit entries to spawns, so the spawn-less 10813 is dropped and the
// clear skips from Archivist Galford (bit 5) straight to the dead side, never
// fighting the live boss. (Same shape as Ramstein, also a spawn-less credit.)
//
// Fix: a BossRosterRegistry OBJECTIVE at Dathrohan's static spawn (bit 6, right
// after Galford), carrying anchored event 5. The objective only travels the tank
// in; an Objective completes on arrival WITHOUT engaging, so the event does the
// fighting: KillCreatureEngage(10812) seeks and pulls Dathrohan, then
// KillCreatureEngage(10813) finishes the transformed Balnazzar.
//
// Two steps, not one: a lone KillCreatureEngage(10812) would report Done the
// instant he transforms at 40% (no live 10812 remains) while Balnazzar is still
// up — the tank would latch the objective and walk off mid-fight. Step 2 holds
// until 10813 is actually dead. Non-persistent: both steps are idempotent
// kill-gates (no WaitForSpawn to false-complete across a combat gap, unlike the
// Slaughterhouse), and the whole fight is one continuous combat at a fixed spot,
// so a restart-from-0 after any combat lull just re-evaluates correctly.
//
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
//   ~20s later); clearing those opens the door to Baron Rivendare (10440). The
//   Mindless charge the party, but the Black Guards walk to fixed posts and idle
//   (they only aggro by proximity), so wave 2 uses KillCreatureEngage to actively
//   SEEK and engage them — otherwise a wave-1 fight that drifted the party off down
//   the hall leaves the guards standing unaggroed and the Baron door never opens (a
//   centre ClearRadius can't see them and a MoveTo hold can't haul the tank back the
//   length of the hall). This whole chain is ONE
//   persistent anchored event tied to a Slaughter-Square
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
    // --- live side: Dathrohan -> Balnazzar -------------------------------
    constexpr uint32 STR_DATHROHAN = 10812;  // Grand Crusader Dathrohan (spawned)
    constexpr uint32 STR_BALNAZZAR = 10813;  // Balnazzar (UpdateEntry, no spawn)
    constexpr float STR_DATH_SEEK = 60.0f;   // engage-seek radius from the anchor
    constexpr uint32 STR_DATH_TIMEOUT = 120000;  // 2 min per phase

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

    // Monotonic phase doors (instance_stratholme.cpp). _slaughterProgress is NOT
    // exposed via GetData, but these doors are: each opens once and stays open, so
    // they are the bulletproof phase signal that survives the long continuous
    // combat (the event engine is dormant in combat) and bridges the multi-second
    // inter-wave lulls that would otherwise let a creature gate complete early.
    constexpr uint32 STR_GO_BARON_DOOR = 175796;   // opens when the 5 guards die
    constexpr uint32 STR_GO_OPEN = 0;              // GO_STATE_ACTIVE (open)
    // The Baron door sits ~51yd north of the hall centre; the default GO search is
    // only 20yd, so the wait needs an explicit reach that finds it from anywhere
    // in the hall.
    constexpr float STR_DOOR_SEARCH = 120.0f;

    // Slaughterhouse-hall centre. NOT the old SlaughterPos (4032,-3378): that sits
    // at the north end RIGHT AT the still-closed Baron door (175796 @ -3364) and
    // doors4 (175405 @ -3389), so the approach hit the door and the tank deadlocked
    // ("closed door blocking path"). This centre is pulled ~37yd SOUTH into the
    // abomination field (they span Y -3380..-3444), reachable from the south
    // entrance gate (175373 @ -3469) without crossing any closed door. The radius
    // covers the whole hall; the zBand keeps the ramps/Baron landing out.
    constexpr float STR_SLAUGHTER_X = 4032.0f;
    constexpr float STR_SLAUGHTER_Y = -3415.0f;
    constexpr float STR_SLAUGHTER_Z = 118.0f;
    // Covers the whole hall with margin (farthest abomination spawn ~68yd) so a
    // patrolling straggler can't drift outside and false-complete the gate while
    // it's still alive (the undead wave spawns only once EVERY abomination dies,
    // so a missed one would deadlock the next step). Bosses (Baron) are excluded
    // from ClearRadius, and the gauntlet behind is already cleared, so a wide
    // radius is safe.
    constexpr float STR_SLAUGHTER_RADIUS = 80.0f;
    constexpr float STR_SLAUGHTER_ZBAND = 15.0f;

    // The whole slaughter chain runs minutes through continuous combat, so every
    // step gets a generous wall-clock timeout (the step timer is not paused in
    // combat — cf. ZulFarrak's 15-min wave step) to keep a long fight from
    // escalating to a stall.
    constexpr uint32 STR_PHASE_TIMEOUT = 300000;   // 5 min per phase
    constexpr uint32 STR_WAVE_GAP_TIMEOUT = 120000;  // 2 min to bridge a wave gap

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

namespace
{
    bool StrZig1(Player* bot, AiObjectContext* context);
    bool StrZig2(Player* bot, AiObjectContext* context);
    bool StrZig3(Player* bot, AiObjectContext* context);
}

void RegisterStratholmeEvents(std::vector<DungeonEvent>& out)
{
    // --- three ziggurat acolyte clears (conditional, fire on door-open) ----
    // ClearRadius (position-based) rather than KillCreature(10399): it drives
    // the tank into the chamber and clears whatever guards the crystal without
    // depending on the exact acolyte count, and the zBand keeps the chamber's
    // adjacent levels out of the clear. (void STR_ACOLYTE — documentary entry.)
    (void) STR_ACOLYTE;
    // STR_RAMSTEIN is documentary too: the slaughter event folds him into step 1's
    // ClearRadius (he is summoned synchronously) rather than naming him in a step.
    (void) STR_RAMSTEIN;

    // --- live side: Grand Crusader Dathrohan -> Balnazzar (anchored, id 5) --
    // Anchored at the Dathrohan objective (BossRosterRegistry OBJ(2), bit 6).
    // Step 1 seeks + engages Dathrohan; at 40% he UpdateEntry's to Balnazzar
    // (same GUID, already in combat), so no live 10812 remains -> step Done.
    // Step 2 holds until Balnazzar himself is dead, then the objective latches.
    out.push_back(EventBuilder(329, 5, "Grand Crusader Dathrohan (Balnazzar)")
                      .Anchored(6)
                      .KillCreatureEngage(STR_DATHROHAN, /*count*/ 1, STR_DATH_SEEK)
                          .Timeout(STR_DATH_TIMEOUT)
                      .KillCreatureEngage(STR_BALNAZZAR, /*count*/ 1, STR_DATH_SEEK)
                          .Timeout(STR_DATH_TIMEOUT)
                      .Build());

    out.push_back(EventBuilder(329, 1, "Ziggurat 1 acolytes (Baroness)")
                      .Conditional(&StrZig1)
                      .PanelBeforeBoss(STR_NERUBENKAN)
                      .ClearRadius(STR_ZIG1_X, STR_ZIG1_Y, STR_ZIG1_Z,
                                   STR_ZIG_RADIUS, STR_ZIG_ZBAND)
                      .Build());

    out.push_back(EventBuilder(329, 2, "Ziggurat 2 acolytes (Nerub'enkan)")
                      .Conditional(&StrZig2)
                      .PanelBeforeBoss(STR_MALEKI)
                      .ClearRadius(STR_ZIG2_X, STR_ZIG2_Y, STR_ZIG2_Z,
                                   STR_ZIG_RADIUS, STR_ZIG_ZBAND)
                      .Build());

    out.push_back(EventBuilder(329, 3, "Ziggurat 3 acolytes (Maleki)")
                      .Conditional(&StrZig3)
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
                      // 1. Clear the hall of the pre-spawned abominations. The last
                      //    one's death SYNCHRONOUSLY summons Ramstein the Gorger
                      //    (10439) right here, so the same ClearRadius rolls straight
                      //    onto him (he is not in the boss list, so it treats him as
                      //    clearable) — no empty-hall gap, no separate wait that could
                      //    false-complete. Done once abominations AND Ramstein die.
                      .ClearRadius(STR_SLAUGHTER_X, STR_SLAUGHTER_Y, STR_SLAUGHTER_Z,
                                   STR_SLAUGHTER_RADIUS, STR_SLAUGHTER_ZBAND)
                          .Timeout(STR_PHASE_TIMEOUT)
                      // 2. Wave 1: Ramstein's death spawns 33 Mindless Undead (11030)
                      //    after a ~5s lull. Wait for the first to appear (a genuine
                      //    out-of-combat lull, so the spawn can't be missed), then
                      //    ClearRadius them — they charge the hall, so the radius +
                      //    reactive combat reap them; count-tolerant (TEMPSUMMON).
                      .WaitForSpawn(STR_MINDLESS, /*alive*/ true)
                          .Timeout(STR_WAVE_GAP_TIMEOUT)
                      .ClearRadius(STR_SLAUGHTER_X, STR_SLAUGHTER_Y, STR_SLAUGHTER_Z,
                                   STR_SLAUGHTER_RADIUS, STR_SLAUGHTER_ZBAND)
                          .Timeout(STR_PHASE_TIMEOUT)
                      // 3. Wave 2: ~20s after the undead die, 5 Black Guards (10394)
                      //    spawn at the north door, WALK to fixed posts ~8yd north of
                      //    the hall centre, then idle there (SetHomePosition). Unlike the
                      //    charging Mindless they never seek the party — they only aggro
                      //    by proximity, and their deaths are what open the Baron door.
                      //    If the wave-1 fight dragged the party off down the hall, the
                      //    guards reach their posts and stand there unaggroed and the run
                      //    wedges. A point-anchored ClearRadius can't recover this: its
                      //    engage scan is bot-centred (so far-off guards are invisible)
                      //    and a MoveTo hold can't haul the tank the length of the hall
                      //    (intra-room hop only — it just sat off-position till timeout).
                      //    So WaitForSpawn first (the kill gate can't false-complete
                      //    before they appear), then KillCreatureEngage: the engage
                      //    pipeline (EngageDirect, long-range walk-in) actively SEEKS the
                      //    guards wherever the party ended up and fights through all five.
                      //    Same mechanism ZulFarrak uses to walk the tank to its bosses.
                      .WaitForSpawn(STR_BLACK_GUARD, /*alive*/ true)
                          .Timeout(STR_WAVE_GAP_TIMEOUT)
                      .KillCreatureEngage(STR_BLACK_GUARD, /*count*/ 1, /*searchRadius*/ 250.0f)
                          .Timeout(STR_PHASE_TIMEOUT)
                      // 4. The guards' death opens the Baron door (175796). Gate on
                      //    that monotonic, combat-gap-proof signal — the authoritative
                      //    "slaughterhouse done" — not a transient creature check.
                      //    Baron (DBC bit 12) is then a normal boss anchor.
                      .WaitForGOState(STR_GO_BARON_DOOR, STR_GO_OPEN,
                                      /*timeoutMs*/ STR_WAVE_GAP_TIMEOUT,
                                      /*searchRadius*/ STR_DOOR_SEARCH)
                      .Build());
}

