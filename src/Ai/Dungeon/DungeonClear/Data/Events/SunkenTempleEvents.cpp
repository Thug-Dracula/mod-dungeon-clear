/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- Sunken Temple / Temple of Atal'Hakkar (map 109) ----------------------
// The most mechanically dense dungeon in the set: THREE independent scripted
// gates over four stacked Z-levels around a central pit. Full design in
// deployment-files/docs/mod-dungeon-clear_sunken-temple-events_plan.md.
//
// GATE 1 (required) — the FORCEFIELD to Jammal'an. Six Atal'ai "defender"
//   mini-bosses ring the upper level (Z ≈ −66.8); each death sets instance
//   data, and the 6th drops the forcefield (GO 149431). They are NOT
//   DungeonEncounters (the auto-roster treats them as trash) and sit ~186yd
//   from Jammal'an, so normal flow doesn't guarantee the gate opens.
//
//   CRITICAL navigation note (live-verified 2026-06-14): the defenders stand on
//   an upper BALCONY ring whose only walkable approach is a long, winding route
//   AROUND the room — the balcony sits high above a floor-level room that shares
//   its X,Y. A raw MoveTo/EngageDirect (base PathGenerator, 74-waypoint cap)
//   can't build that whole ascent: it overruns the cap, the path resolves
//   INCOMPLETE, and the tank walks to the wrong floor-level room directly under
//   the balcony instead of up the ring. So the forcefield CANNOT be a conditional
//   KillCreatureEngage chain (that drives EngageDirect, which bee-lines into the
//   wrong room). It MUST be reached by BOSS-NAV: the LongRangePathfinder (no hop
//   cap, used only for roster anchors) resolves the full multi-level ring route.
//   Hence three OBJECTIVE anchors on the ring (events 1/12/13), each killing one
//   adjacent pair of defenders — boss-nav drives the ascent and the long
//   inter-anchor ring travel; only the short ~70yd same-level within-pair hop
//   falls to EngageDirect. Same lesson as the statue rim walk (§5/§6 Watch B).
//
// GATE 2 (optional pit wing) — the STATUE order puzzle -> Atal'alarion. Six
//   statues on the middle ring must be clicked IN ENTRY ORDER (148830..148835);
//   at statuePhase 6 Atal'alarion un-phases at the pit bottom and the Idol of
//   Hakkar is summoned. Each statue is its OWN objective anchor (boss-nav does
//   the long rim walk) carrying a one-step UseGO event (events 2..7).
//
// GATE 3 (optional, hardest) — the AVATAR of Hakkar. Atal'alarion's death makes
//   the idol usable (event 8); using it summons the Avatar fight in the far
//   NORTH flame room (event 9, Optional + Persistent).
//
// ORDERING TRAP — Weaver/Dreamscythe spawn phaseMask 2 (invisible) until
//   Jammal'an dies, and Atal'alarion is bit 0 (visited first by default) but is
//   the optional, puzzle-gated pit boss. The DBC bit order is NOT a valid clear
//   order. BossRosterRegistry (map 109) removes those three from their low bits
//   and re-adds them as objective anchors ordered after their un-phase trigger,
//   each carrying a KillCreature(engage) event (events 10/11) so the real kill
//   still flips the real DBC bit. See that file + §9 of the plan.

namespace
{
    // GATE 1 — the six Atal'ai defenders (upper balcony ring, Z ≈ −66.8), in
    // ring order. Killing all six drops the forcefield. Each is a mini-boss with
    // adds/totems; the kill steps get a generous timeout (approach + fight).
    constexpr uint32 ST_ZOLO = 5712;
    constexpr uint32 ST_GASHER = 5713;
    constexpr uint32 ST_LORO = 5714;
    constexpr uint32 ST_HUKKU = 5715;
    constexpr uint32 ST_ZULLOR = 5716;
    constexpr uint32 ST_MIJAN = 5717;
    constexpr uint32 ST_DEFENDER_TIMEOUT = 120000;  // 120s per defender
    // Comfortably covers the ~70yd hop to the anchor's ring-adjacent partner
    // (only ever matches the named entry, so no cross-pair pickup).
    constexpr float ST_DEFENDER_SEARCH = 120.0f;

    // GATE 2 — statues (middle ring, Z -148.7), in correct click order.
    constexpr uint32 ST_STATUE_1 = 148830;
    constexpr uint32 ST_STATUE_2 = 148831;
    constexpr uint32 ST_STATUE_3 = 148832;
    constexpr uint32 ST_STATUE_4 = 148833;
    constexpr uint32 ST_STATUE_5 = 148834;
    constexpr uint32 ST_STATUE_6 = 148835;
    // Comfortably covers each statue objective's 8yd arrive radius.
    constexpr float ST_STATUE_SEARCH = 12.0f;

    // Reordered real bosses (see BossRosterRegistry map 109).
    constexpr uint32 ST_WEAVER = 5720;
    constexpr uint32 ST_DREAMSCYTHE = 5721;
    constexpr uint32 ST_ATALALARION = 8580;
    constexpr float ST_BOSS_SEARCH = 250.0f;
    // A real boss fight (approach + kill) far exceeds the default step timeout.
    constexpr uint32 ST_BOSS_TIMEOUT = 180000;  // 3 min

    // GATE 3 — Avatar of Hakkar (north flame room, summoned).
    constexpr uint32 ST_IDOL_GO = 148838;
    constexpr uint32 ST_NIGHTMARE_SUPPRESSOR = 8497;
    constexpr uint32 ST_HAKKARI_BLOODKEEPER = 8438;
    constexpr uint32 ST_AVATAR = 8443;
    constexpr float ST_NORTH_SEARCH = 80.0f;
    constexpr uint32 ST_AVATAR_SPAWN_WAIT = 60000;
}

void RegisterSunkenTempleEvents(std::vector<DungeonEvent>& out)
{
    // --- Events 1/12/13: GATE 1 — drop the forcefield (3 ring anchors) ------
    // Three Anchored + Persistent events, one per objective anchor on the upper
    // balcony ring (BossRosterRegistry orders them 0/1/2, before Jammal'an).
    // Boss-nav walks the tank UP the ring to each anchor (the long multi-level
    // ascent a raw MoveTo can't build); each event then engage-kills the anchor's
    // defender plus its ring-adjacent partner (~70yd, a short same-level hop
    // EngageDirect handles). Visited in a monotonic sweep around the ring:
    //   A (Mijan, Zul'Lor) -> B (Zolo, Gasher) -> C (Loro, Hukku).
    // Required: if the gate won't open the run should stall for the human rather
    // than skip a wall. A defender already swept by trash no-ops instantly (its
    // KillCreature gate is Done with no live target). The instance drops GO 149431
    // when all six die — no WaitForGOState confirm (the tank ends on the far ring
    // where that GO's grid may be cold, so a GO scan would false-timeout).
    out.push_back(EventBuilder(109, 1, "Atal'ai Defenders (Mijan & Zul'Lor)")
                      .Anchored(0)
                      .Persistent()
                      .KillCreatureEngage(ST_MIJAN, 1, ST_DEFENDER_SEARCH).Timeout(ST_DEFENDER_TIMEOUT)
                      .KillCreatureEngage(ST_ZULLOR, 1, ST_DEFENDER_SEARCH).Timeout(ST_DEFENDER_TIMEOUT)
                      .Build());
    out.push_back(EventBuilder(109, 12, "Atal'ai Defenders (Zolo & Gasher)")
                      .Anchored(1)
                      .Persistent()
                      .KillCreatureEngage(ST_ZOLO, 1, ST_DEFENDER_SEARCH).Timeout(ST_DEFENDER_TIMEOUT)
                      .KillCreatureEngage(ST_GASHER, 1, ST_DEFENDER_SEARCH).Timeout(ST_DEFENDER_TIMEOUT)
                      .Build());
    out.push_back(EventBuilder(109, 13, "Atal'ai Defenders (Loro & Hukku)")
                      .Anchored(2)
                      .Persistent()
                      .KillCreatureEngage(ST_LORO, 1, ST_DEFENDER_SEARCH).Timeout(ST_DEFENDER_TIMEOUT)
                      .KillCreatureEngage(ST_HUKKU, 1, ST_DEFENDER_SEARCH).Timeout(ST_DEFENDER_TIMEOUT)
                      .Build());

    // --- Events 2-7: GATE 2 — the six statue clicks ------------------------
    // One Anchored event per statue: the objective anchor (BossRosterRegistry)
    // sits AT the statue so boss-nav does the long rim walk; this event just
    // clicks once the tank has arrived. Optional — the whole pit wing is bonus,
    // so a failed click skips rather than stalling the (already-finished)
    // required spine. Out-of-order is impossible (anchors are visited in roster
    // order) and a mis-click is harmless anyway (the statue phase never regresses).
    out.push_back(EventBuilder(109, 2, "Atal'ai Statue 1")
                      .Anchored(10).UseGO(ST_STATUE_1, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 3, "Atal'ai Statue 2")
                      .Anchored(11).UseGO(ST_STATUE_2, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 4, "Atal'ai Statue 3")
                      .Anchored(12).UseGO(ST_STATUE_3, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 5, "Atal'ai Statue 4")
                      .Anchored(13).UseGO(ST_STATUE_4, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 6, "Atal'ai Statue 5")
                      .Anchored(14).UseGO(ST_STATUE_5, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 7, "Atal'ai Statue 6")
                      .Anchored(15).UseGO(ST_STATUE_6, ST_STATUE_SEARCH).Optional().Build());

    // --- Event 8: GATE 3 beat 1 — Awaken the Soulflayer (Idol of Hakkar) ---
    // Anchored at the idol (ordered right after Atal'alarion so it is usable on
    // arrival — his death removes the idol's NOT_SELECTABLE). Using it summons the
    // Avatar fight in the north room NOW; the Avatar anchor (event 9) is the very
    // next objective so boss-nav heads north immediately (the Avatar despawns out
    // of combat). Optional — pit-wing bonus.
    out.push_back(EventBuilder(109, 8, "Awaken the Soulflayer (Idol of Hakkar)")
                      .Anchored(17).UseGO(ST_IDOL_GO, 20.0f).Optional().Build());

    // --- Event 9: GATE 3 beat 2 — the Avatar of Hakkar fight ---------------
    // Anchored in the north flame room, Optional + Persistent. Clear the
    // channelers, wait for the Avatar to manifest, then kill it. UNVERIFIED from
    // static DB: the channelers respawn on a counter and the Avatar may only be
    // vulnerable past a threshold — the two engage steps are a first cut. Optional
    // means any channeler-loop hang degrades to `dc skip`; Persistent keeps
    // progress across the channeler/Avatar combat gaps and lets the tank roam the
    // room. (If live runs demand it, convert to a Conditional+Repeatable
    // "clear channelers while Avatar absent" loop, Razorfen-gong shape.)
    out.push_back(EventBuilder(109, 9, "Avatar of Hakkar")
                      .Anchored(18)
                      .Persistent()
                      .Optional()
                      .KillCreatureEngage(ST_NIGHTMARE_SUPPRESSOR, 1, ST_NORTH_SEARCH)
                      .KillCreatureEngage(ST_HAKKARI_BLOODKEEPER, 1, ST_NORTH_SEARCH)
                      .WaitForSpawn(ST_AVATAR, /*wantAlive*/ true, ST_AVATAR_SPAWN_WAIT)
                      .KillCreatureEngage(ST_AVATAR, 1, ST_NORTH_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .Build());

    // --- Event 10: Weaver & Dreamscythe (required spine) -------------------
    // Both spawn phaseMask 2 until Jammal'an dies; this objective is ordered after
    // him so they are visible on arrival. They sit ~10yd apart and un-phase
    // together, so ONE objective kills both (two same-index objectives would be
    // skipped by NextDungeonBossValue's strictly-greater advance-forward). Two
    // engage steps; the real kills flip their real DBC bits. Persistent so the
    // back-to-back fights don't rewind the chain; Required (spine boss).
    out.push_back(EventBuilder(109, 10, "Weaver & Dreamscythe")
                      .Anchored(5)
                      .Persistent()
                      .KillCreatureEngage(ST_WEAVER, 1, ST_BOSS_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .KillCreatureEngage(ST_DREAMSCYTHE, 1, ST_BOSS_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .Build());

    // --- Event 11: Atal'alarion (optional pit wing) ------------------------
    // Un-phases at the pit bottom once statuePhase hits 6 (after statue 6). One
    // engage step kills him (flipping his real bit); his death makes the idol
    // usable (event 8). Optional + Persistent — pit-wing bonus, and the deep pit
    // may be unmeshed (water-surface navmesh), in which case an unreachable boss
    // degrades to `dc skip` after the kill step times out.
    out.push_back(EventBuilder(109, 11, "Atal'alarion")
                      .Anchored(16)
                      .Persistent()
                      .Optional()
                      .KillCreatureEngage(ST_ATALALARION, 1, ST_BOSS_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .Build());
}
