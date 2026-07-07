/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// --- Old Hillsbrad Foothills / Escape from Durnholde (map 560) --------------
//
// The whole zone is one long scripted chain (core: old_hillsbrad.cpp +
// instance_old_hillsbrad.cpp + boss_captain_skarloc.cpp). The instance exposes a
// MONOTONIC progress counter, GetData(DATA_ESCORT_PROGRESS) (data id 0), which is
// the single source of truth we gate on:
//
//   0 NONE · 1 BARRELS (5 bombs planted, Lt. Drake summoned) · 3 THRALL_ARMORED ·
//   4 AMBUSHES_1 · 5 SKARLOC_KILLED · 6 TARREN_MILL · 7 TARETHA_MEET ·
//   9 FINISHED (Epoch Hunter dead).
//
// Three anchored objectives sequence the run:
//   (1) Ride to the keep      — gossip Brazen, then relocate the party there.
//   (2) Distraction + Drake   — bomb 5 barrels (item-on-GO), kill Lt. Drake.
//   (3) Free & escort Thrall  — ONE EscortCreature step that runs from freeing
//       Thrall all the way to Epoch Hunter's death, driven entirely off Thrall's
//       gossip flag (release / mount-up / start-waves / Epoch cutscene) and the
//       instance progress counter. The escort driver assists every ambush, the
//       Skarloc fight, the barn/church/inn waves, the Epoch waves and Epoch
//       himself, and speed-matches Thrall on the mounted Tarren Mill ride.
//
// The three bosses (Lt. Drake 17848, Capt. Skarloc 17862, Epoch Hunter 18096) are
// TempSummons (spawnId 0), invisible to the spawn store, so there is no derived
// roster — the objectives ARE the clear. Drake is engaged by objective (2)'s
// KillCreatureEngage; Skarloc and Epoch are engaged by objective (3)'s escort
// threat-assist (both attack Thrall). No standalone boss anchors — a boss anchor
// would have DC navigate to them independently and fight the escort.

namespace
{
    // Instance progress counter (old_hillsbrad.h DATA_ESCORT_PROGRESS) + the
    // milestone values we gate the escort completion on.
    constexpr uint32 OH_DATA_ESCORT_PROGRESS = 0;
    constexpr uint32 OH_PROGRESS_FINISHED    = 9;  // ENCOUNTER_PROGRESS_FINISHED

    // Entrance / Brazen (flight-master drake) — where the party spawns in.
    constexpr float OH_ENTRANCE_X = 2381.79f;
    constexpr float OH_ENTRANCE_Y = 1170.31f;
    constexpr float OH_ENTRANCE_Z = 66.54f;
    constexpr uint32 NPC_BRAZEN   = 18725;

    // Erozion — hands out the Pack of Incendiary Bombs (item 25853). His grant
    // gossip is quest-gated (quest 10283 taken/rewarded), which a bot never has,
    // AND Brazen's ride gossip (menu 7959) only appears once the player HOLDS item
    // 25853 — so the bot must acquire the pack before Brazen will transport it.
    // Since Erozion's option is unreachable for a questless bot, hook 3 grants the
    // pack directly (mechanically identical to Erozion's AddItem). See
    // ObjectiveHookRegistry GrantIncendiaryBombs. Erozion (18723) sits at:
    constexpr float OH_EROZION_X       = 2375.50f;
    constexpr float OH_EROZION_Y       = 1175.50f;
    constexpr float OH_EROZION_Z       = 65.30f;
    constexpr uint32 OH_HOOK_GRANT_BOMBS = 3;  // ObjectiveHookRegistry id

    // Brazen's drake DROP-OFF — OUTSIDE the keep's west wall (in-game reading), on
    // the road above the courtyard. The party teleports HERE and then WALKS in to
    // the barrels (never teleporting onto them), matching the intended flow.
    constexpr float OH_DROP_X = 2023.73f;
    constexpr float OH_DROP_Y = 241.40f;
    constexpr float OH_DROP_Z = 68.38f;

    // Keep courtyard — central to the barrel cluster; the OBJ(2) anchor the tank
    // walks in to from the drop before bombing (instancePositions[1] gather point).
    constexpr float OH_KEEP_X = 2103.23f;
    constexpr float OH_KEEP_Y = 93.55f;
    constexpr float OH_KEEP_Z = 53.10f;

    // The distraction: item 25853 (Pack of Incendiary Bombs) casts spell 32744
    // (Planting Incendiary Bomb) which the barrel's SmartAI counts. ANY 5 distinct
    // barrels (GO 182589) work — the core has no positional/order check — so these
    // five (a tight SW-courtyard line, E->W) are chosen for nav efficiency. Each
    // step's anchor picks a SPECIFIC barrel so five same-entry GOs are five hits.
    constexpr uint32 OH_ITEM_BOMBS   = 25853;
    constexpr uint32 OH_SPELL_PLANT  = 32744;
    constexpr uint32 OH_GO_BARREL    = 182589;
    constexpr uint32 NPC_LT_DRAKE    = 17848;

    // Thrall's cell (instance-static spawn) — objective (3) anchor.
    constexpr uint32 NPC_THRALL   = 17876;
    constexpr float OH_THRALL_X   = 2231.90f;
    constexpr float OH_THRALL_Y   = 120.00f;
    constexpr float OH_THRALL_Z   = 82.30f;
}

void RegisterOldHillsbradEvents(std::vector<DungeonEvent>& out)
{
    // (1) GET THE BOMBS FROM EROZION, THEN RIDE TO DURNHOLDE KEEP.
    // The player must hold the Pack of Incendiary Bombs (item 25853) before Brazen
    // will offer his drake ride (Brazen menu 7959 opt 0 is condition-gated on the
    // item). Erozion normally grants it, but his gossip is quest-gated (quest 10283)
    // and unreachable for a questless bot — so walk to Erozion and grant the pack
    // directly (hook 3), THEN gossip Brazen to trigger the ride (spell 32892), THEN
    // TeleportParty the WHOLE party to the keep. The taxi flies only the invoker, so
    // a lone-leader flight would desync the followers 1100yd back; the teleport
    // lands everyone together at Brazen's DROP-OFF outside the walls (its
    // MotionMaster Clear cancels the just-started flight). The party then WALKS in
    // to the barrels via the OBJ(2) navigation — never teleported onto them.
    // PERSISTENT so the at-objective trigger stays sticky after the gossip nudges
    // the leader off the anchor — the TeleportParty then still fires; generous
    // checkpoint radius so a tick or two of flight still satisfies "at the checkpoint".
    out.push_back(
        EventBuilder(560, 1, "Ride to Durnholde Keep")
            .Anchored(/*orderIndex*/ 1)
            .Persistent()
            .MoveTo(OH_EROZION_X, OH_EROZION_Y, OH_EROZION_Z, /*radius*/ 6.0f)
            .Custom(OH_HOOK_GRANT_BOMBS)  // hold the pack -> Brazen's ride option appears
            .Gossip(NPC_BRAZEN, /*option*/ 0, /*searchRadius*/ 20.0f)
                .SkipIfTargetMissing()
            .TeleportParty(/*checkpoint*/ OH_ENTRANCE_X, OH_ENTRANCE_Y, OH_ENTRANCE_Z,
                           /*landing (drop-off outside the walls)*/ OH_DROP_X, OH_DROP_Y, OH_DROP_Z,
                           /*radius*/ 200.0f)
            .Build());

    // (2) CREATE A DISTRACTION, THEN SLAY LIEUTENANT DRAKE.
    // Bomb five barrels; the 5th plant fires the instance's summon of Lt. Drake
    // (~18s later) up on the keep platform. WaitForSpawn(Drake) is the real "5
    // counted" check (a same-barrel double-hit never advances the count, so it
    // would time out loudly rather than pass). KillCreatureEngage seeks up to the
    // platform and kills him. PERSISTENT: the Drake fight is a >1s combat gap that
    // would otherwise rewind the step chain and re-bomb the barrels.
    out.push_back(
        EventBuilder(560, 2, "Create a distraction and slay Lieutenant Drake")
            .Anchored(/*orderIndex*/ 2)
            .Persistent()
            // Step 0: a lenient MoveTo at the courtyard anchor. Its only job is to
            // reach stepIndex 1 so the persistent event goes STICKY (roam-enabled)
            // — otherwise driving to the first barrel moves the tank off the anchor
            // while stepIndex is still 0, the at-objective gate closes, and the
            // advance action fights the barrel driver back to the anchor. The radius
            // is generous because the exact anchor spot is blocked ~13yd out (the
            // tank can't stand on it), so we treat "near the courtyard" as arrived.
            .MoveTo(OH_KEEP_X, OH_KEEP_Y, OH_KEEP_Z, /*radius*/ 30.0f)
            // Spell-only (the barrel's SmartAI SPELLHIT does the SetData — one count
            // each). Tight cast reach (executor default 8yd) forces the tank to walk
            // INTO each house rather than plink distant barrels from the first one.
            .UseItemOnGO(OH_ITEM_BOMBS, OH_SPELL_PLANT, OH_GO_BARREL, 2119.21f, 42.49f, 53.78f)
            .UseItemOnGO(OH_ITEM_BOMBS, OH_SPELL_PLANT, OH_GO_BARREL, 2108.03f, 54.95f, 53.65f)
            .UseItemOnGO(OH_ITEM_BOMBS, OH_SPELL_PLANT, OH_GO_BARREL, 2100.11f, 43.54f, 53.56f)
            .UseItemOnGO(OH_ITEM_BOMBS, OH_SPELL_PLANT, OH_GO_BARREL, 2080.19f, 64.74f, 53.88f)
            .UseItemOnGO(OH_ITEM_BOMBS, OH_SPELL_PLANT, OH_GO_BARREL, 2067.49f, 106.07f, 54.61f)
            .WaitForSpawn(NPC_LT_DRAKE, /*wantAlive*/ true, /*timeout*/ 60000)
            .KillCreatureEngage(NPC_LT_DRAKE, /*count*/ 1, /*searchRadius*/ 200.0f)
            .Build());

    // (3) FREE THRALL AND ESCORT HIM TO FREEDOM.
    // ONE EscortCreature step, gated on the progress counter reaching FINISHED (9)
    // — Epoch Hunter dead. The escort driver:
    //   * gossips Thrall whenever he raises his gossip flag (release from the cell,
    //     mount up after Skarloc, start the Tarren Mill waves, the Epoch cutscene,
    //     and to resume after a death-reset) — his single "talk to continue" signal;
    //   * follows + engages every attacker (prison-camp ambushes, Captain Skarloc,
    //     the barn/church/inn guard waves, the three Infinite waves, and Epoch
    //     Hunter himself — all of which attack Thrall);
    //   * speed-matches the party to Thrall's 1.6x mount on the long Tarren Mill
    //     ride so his npc_escortAI never resets him for the party falling >100yd back.
    // Wide escortee scan (150yd) so the mounted ride never loses him off-grid; a
    // slightly wider threat radius (25yd) catches the waves that rush him.
    // PERSISTENT: the escort spans a dozen+ combat gaps that would rewind a normal
    // anchored event. Step 0 (a short MoveTo to the cell) bumps stepIndex to 1 so
    // the persistence sticky-trigger engages and the tank can roam the whole zone
    // from the anchor while the escort drives.
    out.push_back(
        EventBuilder(560, 3, "Free Thrall and escort him to freedom")
            .Anchored(/*orderIndex*/ 3)
            .Persistent()
            .MoveTo(OH_THRALL_X, OH_THRALL_Y, OH_THRALL_Z, /*radius*/ 8.0f)
            .EscortCreature(/*escortee*/ NPC_THRALL, /*startGossipOption*/ 0,
                            /*doneEntry*/ 0, /*doneBit*/ -1,
                            /*standoff*/ 5.0f, /*threatRadius*/ 25.0f,
                            /*threatZBand*/ 20.0f, /*searchRadius*/ 150.0f,
                            /*doneDataId*/ static_cast<int32>(OH_DATA_ESCORT_PROGRESS),
                            /*doneDataMin*/ OH_PROGRESS_FINISHED)
            .Build());
}

// --- roster patch: the three objectives (no derived roster; bosses are summons) --
void RegisterOldHillsbradRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = 560;
    p.add = {
        MakeObjective(OBJ(1), /*encounterIndex*/ 1, 560, "Ride to Durnholde Keep",
                      OH_ENTRANCE_X, OH_ENTRANCE_Y, OH_ENTRANCE_Z, /*arriveRadius*/ 12.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1, /*orderOverride*/ 1),
        MakeObjective(OBJ(2), /*encounterIndex*/ 2, 560,
                      "Create a distraction and slay Lieutenant Drake",
                      OH_KEEP_X, OH_KEEP_Y, OH_KEEP_Z, /*arriveRadius*/ 25.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 2, /*orderOverride*/ 2),
        MakeObjective(OBJ(3), /*encounterIndex*/ 3, 560, "Free Thrall and escort him to freedom",
                      OH_THRALL_X, OH_THRALL_Y, OH_THRALL_Z, /*arriveRadius*/ 12.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 3, /*orderOverride*/ 3),
    };
    t.push_back(std::move(p));
}
