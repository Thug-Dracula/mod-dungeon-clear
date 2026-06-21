/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BossRosterRegistry.h"

#include <algorithm>
#include <unordered_set>

static_assert(BossRosterRegistry::ObjectiveEntry(0) == 0x4F000000u,
              "objective entry base must stay in the synthetic range");

namespace
{
    // Synthetic entry for the seq-th non-creature objective on a map. The shared
    // definition lives in the header (BossRosterRegistry::ObjectiveEntry) so an
    // event file can name the objective it sorts relative to in the status panel
    // (panelGatesBossEntry); OBJ is the local shorthand used by the table below.
    constexpr uint32 OBJ(uint32 seq) { return BossRosterRegistry::ObjectiveEntry(seq); }

    // Build a boss anchor with an inherited completion bit. `completionFrom` is
    // the auto-derived entry whose encounterIndex (kill-bit) this boss borrows
    // — used when the real boss has no DungeonEncounter row of its own.
    // `orderOverride` (default -1) reorders the boss in the clear sequence
    // independently of its kill-bit — see DungeonBossInfo::orderOverride. Use it
    // when re-adding a real boss whose DBC bit doesn't match the desired path
    // (pass completionFrom = the boss's own entry to keep its real kill-bit).
    DungeonBossInfo MakeBoss(uint32 entry, uint32 mapId, char const* name,
                             float x, float y, float z, uint32 completionFrom,
                             int32 orderOverride = -1)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.name = name;
        b.mapId = mapId;
        b.x = x;
        b.y = y;
        b.z = z;
        b.kind = DungeonAnchorKind::Boss;
        b.inheritCompletionFrom = completionFrom;
        b.orderOverride = orderOverride;
        return b;
    }

    // Build a travel-objective anchor. `encounterIndex` slots it into the
    // encounter ordering; `gateEntry` (optional) is a creature whose live
    // presence also satisfies the objective; `hook` (optional) is an
    // ObjectiveHookRegistry id. `orderOverride` (default -1) reorders the
    // objective in the clear sequence independently of `encounterIndex` (which,
    // for an objective, is only an ordering hint — objectives carry no real
    // kill-bit), so it can share a 1..N key scale with reordered real bosses.
    DungeonBossInfo MakeObjective(uint32 entry, uint32 encounterIndex, uint32 mapId,
                                  char const* name, float x, float y, float z,
                                  float arriveRadius = 0.0f, uint32 gateEntry = 0,
                                  uint32 hook = 0, uint32 eventId = 0,
                                  int32 orderOverride = -1)
    {
        DungeonBossInfo o;
        o.entry = entry;
        o.encounterIndex = encounterIndex;
        o.name = name;
        o.mapId = mapId;
        o.x = x;
        o.y = y;
        o.z = z;
        o.kind = DungeonAnchorKind::Objective;
        o.arriveRadius = arriveRadius;
        o.gateEntry = gateEntry;
        o.onArriveHook = hook;
        o.eventId = eventId;
        o.orderOverride = orderOverride;
        return o;
    }

    // ---------------------------------------------------------------------
    // The patch table. Add one row per dungeon that needs a roster fix.
    // ---------------------------------------------------------------------
    std::vector<BossRosterPatch> const& PatchTable()
    {
        static std::vector<BossRosterPatch> const kPatches = []
        {
            std::vector<BossRosterPatch> t;

            // --- Scarlet Monastery: Cathedral (map 189) ------------------
            // The derived list ends at High Inquisitor Whitemane (3977), but
            // she is only attackable AFTER Scarlet Commander Mograine is
            // engaged (the SmartAI event-resurrect fight). The real pull
            // target, Mograine (3976), has NO DungeonEncounter row of his own,
            // so he never appears and has no kill-bit. Targeting Whitemane's
            // anchor stalls the tank.
            //
            // Fix: drop Whitemane, add Mograine at his spawn (verified from the
            // creature table, map 189), and have Mograine borrow Whitemane's
            // encounterIndex via inheritCompletionFrom — completing the
            // Cathedral encounter (Whitemane's bit) then drops Mograine from the
            // list through the existing NextDungeonBossValue mask logic.
            // RoomAggroRegistry already flags 3976, so the room pre-clear fires.
            //
            // ORDER FIX. Whitemane's DBC bit (5) sorts Mograine AFTER High
            // Inquisitor Fairbanks (4542, bit 4), so the auto path was
            // Fairbanks -> room-clear + Mograine. The run is much smoother the
            // other way: sweep the Reanimation chamber and kill Mograine FIRST,
            // then mop up Fairbanks last (he stands off in his own alcove and
            // pulls nothing of the main hall). Give Mograine orderOverride 3 so
            // he is picked before Fairbanks while still completing on Whitemane's
            // real kill-bit 5 (BossOrderKey uses the override; the completion mask
            // still keys on encounterIndex) — same decoupling as Stratholme's
            // Barthilas. Net Cathedral order: room-clear + Mograine -> Fairbanks.
            {
                BossRosterPatch p;
                p.mapId = 189;
                p.remove = { 3977 };
                p.add = {
                    MakeBoss(3976, 189, "Scarlet Commander Mograine",
                             1153.9f, 1398.4f, 32.6f, /*completionFrom*/ 3977,
                             /*orderOverride*/ 3),
                };
                t.push_back(std::move(p));
            }

            // --- Scholomance (map 289) — Marduk & Vectus, one merged boss ---
            // Marduk Blackpool (10433) and Vectus (10432) are two separate
            // DungeonEncounters that share one room ~18yd apart and are wired
            // together in SmartAI: each "On Aggro -> Set Data 3" and "On Data Set
            // 3 -> Attack Start", so pulling EITHER pulls BOTH. They do not
            // pre-aggro, but they stand in a chamber of ~32 Scholomance Students;
            // engaging them before the room is clear (or AoE-waking one while
            // clearing) drags the whole pile in.
            //
            // Collapse the pair into ONE boss, mirroring SM Cathedral
            // (Mograine/Whitemane): drop BOTH derived entries and re-add a single
            // "Marduk & Vectus" anchored on VECTUS's spawn (the boss nearest the
            // student cluster — the close trash at ~14yd then falls inside the
            // tracked boss's keep-away sphere and is left "coming with the boss").
            // The merged boss re-uses Vectus's real entry (10432) so the engage
            // pipeline drives a real creature, and inherits Vectus's own kill-bit
            // via inheritCompletionFrom (resolved from the base list before the
            // remove). Killing the linked pair flips that bit -> encounter done.
            // Marduk stays in RoomAggroRegistry (partner exclusion) but off the
            // roster, so the tank never routes to him separately. The room-aggro
            // pre-clear event (ScholomanceEvents.cpp, condition 3) clears the
            // students first.
            {
                BossRosterPatch p;
                p.mapId = 289;
                p.remove = { 10432, 10433 };
                p.add = {
                    MakeBoss(10432, 289, "Marduk & Vectus",
                             143.5f, 99.1f, 104.7f, /*completionFrom*/ 10432),
                };
                t.push_back(std::move(p));
            }

            // --- Sunken Temple (map 109) — full dungeon-events restructure
            // Three scripted gates + a phase-ordering trap (see
            // deployment-files/docs/mod-dungeon-clear_sunken-temple-events_plan.md
            // and SunkenTempleEvents.cpp). The DBC bit order is NOT a valid clear
            // order: Atal'alarion is bit 0 but is the OPTIONAL, puzzle-gated pit
            // boss, and Weaver/Dreamscythe are early bits but spawn phaseMask 2
            // (invisible) until Jammal'an dies. Played straight the tank stalls on
            // an invisible boss at #2 or wastes #0 on the deep optional pit.
            //
            // Fix (the established remove/re-add-as-objective pattern): drop the
            // three phase/puzzle-gated bosses from their low bits and re-add them
            // as OBJECTIVE anchors ordered AFTER their un-phase trigger, each
            // carrying a KillCreature(engage) event so the real kill still flips
            // the real DBC bit (objectives skip the completion-mask check, so
            // their orderIndex is a pure ordering hint that can't collide). The
            // remaining auto bosses — Jammal'an, Morphaz, Hazzas, Eranikus — keep
            // their real bits and natural order.
            //
            // FORCEFIELD via OBJECTIVE anchors (not a conditional event). The six
            // Atal'ai defenders each stand on their OWN separate upper BALCONY
            // whose only walkable approach is a long multi-level route around the
            // room — a raw MoveTo/EngageDirect (74-cap PathGenerator) overruns the
            // cap, resolves INCOMPLETE, and walks the tank to the wrong floor-level
            // room under the balcony (live-verified). Only boss-nav's
            // LongRangePathfinder (no cap) resolves each ring approach, so EACH
            // defender gets its OWN anchor (six total). An earlier design paired
            // two defenders per anchor and engaged the second via EngageDirect;
            // that second, far defender could not be pathed and was skipped — the
            // reported "it skips the second boss of the group" bug.
            //
            // ORDER INDICES (real DBC bits, read from DungeonEncounter.dbc:
            // Jammal'an 3, Morphaz 5, Hazzas 6, Eranikus 8; Atal'alarion 0,
            // Dreamscythe 1, Weaver 2 are removed). Objective indices below:
            //   0      all six forcefield defenders (before Jammal'an 3) — they
            //          share index 0 and are visited as one gate by
            //          NextDungeonBossValue::PickTarget's same-index-first advance
            //          (only the spawned auto boss at bit 0, Atal'alarion, used
            //          this slot, and it is removed). Order among them is the
            //          ring-sweep insertion order; the forcefield drops once all
            //          six die regardless of which order they fall in.
            //   5  Weaver & Dreamscythe (after Jammal'an 3, shares Morphaz's bit 5
            //      so it is reached just before Morphaz via the same same-index
            //      advance) — ONE merged objective (they are ~10yd apart and
            //      un-phase together).
            //   10..15 six statues in click order } the whole OPTIONAL pit wing at
            //   16     Atal'alarion                } the route tail (all > Eranikus
            //   17     Idol of Hakkar               } 8) so a pit pathing failure
            //   18     Avatar of Hakkar             } never blocks the spine.
            {
                BossRosterPatch p;
                p.mapId = 109;
                // Remove the three phase/puzzle-gated bosses; re-added as
                // objectives below. (Weaver 5720, Dreamscythe 5721, Atal'alarion
                // 8580 are auto-derived bosses at bits 2/1/0 respectively.)
                p.remove = { 5720, 5721, 8580 };
                p.add = {
                    // GATE 1 forcefield — SIX ring anchors, one per Atal'ai
                    // defender, in a monotonic sweep around the balcony. Each
                    // defender stands on its OWN separate balcony reachable only
                    // by a long, winding multi-level route, so each needs its own
                    // boss-nav anchor (LongRangePathfinder, no 74-cap) to be
                    // travelled to — a single anchor that then tried to engage a
                    // second, far defender via EngageDirect (raw capped MoveTo)
                    // could not path the trip and SKIPPED that second defender
                    // (the live "it skips the second boss of the group" bug). All
                    // six share encounterIndex 0 so they sort before Jammal'an
                    // (bit 3, the gate they unlock) and are visited as one gate by
                    // NextDungeonBossValue::PickTarget's same-index-first advance;
                    // each carries a one-kill event (1/12/13/14/15/16). Coords are
                    // each defender's own spawn (Z ≈ −66.8). Ring-sweep order
                    // (≈30°→90°→…→330° around centre −467,95).
                    MakeObjective(OBJ(11), /*orderIndex*/ 0, 109, "Atal'ai Defender (Mijan)",
                                  -406.2f, 131.1f, -66.9f, /*arriveRadius*/ 15.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
                    MakeObjective(OBJ(12), /*orderIndex*/ 0, 109, "Atal'ai Defender (Zul'Lor)",
                                  -467.4f, 166.0f, -66.7f, /*arriveRadius*/ 15.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 12),
                    MakeObjective(OBJ(13), /*orderIndex*/ 0, 109, "Atal'ai Defender (Zolo)",
                                  -528.6f, 130.2f, -66.8f, /*arriveRadius*/ 15.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 13),
                    MakeObjective(OBJ(14), /*orderIndex*/ 0, 109, "Atal'ai Defender (Gasher)",
                                  -528.0f, 59.5f, -66.7f, /*arriveRadius*/ 15.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 14),
                    MakeObjective(OBJ(15), /*orderIndex*/ 0, 109, "Atal'ai Defender (Loro)",
                                  -466.7f, 24.4f, -66.8f, /*arriveRadius*/ 15.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 15),
                    MakeObjective(OBJ(16), /*orderIndex*/ 0, 109, "Atal'ai Defender (Hukku)",
                                  -405.5f, 60.5f, -67.1f, /*arriveRadius*/ 15.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 16),

                    // Central circle PRE-CLEAR (orderIndex 1: after the six
                    // defenders at 0, before Jammal'an at 3). The big circular
                    // floor around the central pit is patrolled by dangerous packs
                    // (whelps/wanderers/scalebanes); Weaver & Dreamscythe un-phase
                    // and circle this floor AFTER Jammal'an dies, so it must be
                    // swept FIRST. A ClearRadius event (17) clears every reachable
                    // hostile within 60yd (2D) and a 20yd floor band of the pit
                    // centre — z-banded so the upper defender balconies and the
                    // deep statue/Atal'alarion pit are excluded. arriveRadius 20
                    // puts the tank in the circle before the clear gate evaluates.
                    MakeObjective(OBJ(17), /*orderIndex*/ 1, 109, "Central Circle (pre-clear)",
                                  -467.0f, 95.0f, -91.0f, /*arriveRadius*/ 20.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 17),

                    // Required spine: Weaver + Dreamscythe (un-phased by Jammal'an).
                    // One objective at their midpoint kills both via event 10.
                    MakeObjective(OBJ(1), /*orderIndex*/ 5, 109, "Weaver & Dreamscythe",
                                  -456.2f, 132.5f, -91.2f, /*arriveRadius*/ 30.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 10),

                    // Optional pit wing — six statues in click order (each at its
                    // statue so boss-nav does the long rim walk), Atal'alarion, the
                    // idol, the Avatar. eventIds 2..9 (see SunkenTempleEvents.cpp).
                    MakeObjective(OBJ(2), 10, 109, "Atal'ai Statue 1",
                                  -515.6f, 95.3f, -148.7f, 8.0f, 0, 0, /*eventId*/ 2),
                    MakeObjective(OBJ(3), 11, 109, "Atal'ai Statue 2",
                                  -419.8f, 94.5f, -148.7f, 8.0f, 0, 0, /*eventId*/ 3),
                    MakeObjective(OBJ(4), 12, 109, "Atal'ai Statue 3",
                                  -491.4f, 136.0f, -148.7f, 8.0f, 0, 0, /*eventId*/ 4),
                    MakeObjective(OBJ(5), 13, 109, "Atal'ai Statue 4",
                                  -491.5f, 53.5f, -148.7f, 8.0f, 0, 0, /*eventId*/ 5),
                    MakeObjective(OBJ(6), 14, 109, "Atal'ai Statue 5",
                                  -443.9f, 136.1f, -148.7f, 8.0f, 0, 0, /*eventId*/ 6),
                    MakeObjective(OBJ(7), 15, 109, "Atal'ai Statue 6",
                                  -443.4f, 53.8f, -148.7f, 8.0f, 0, 0, /*eventId*/ 7),
                    // Atal'alarion un-phases at statuePhase 6; killed via event 11.
                    MakeObjective(OBJ(8), 16, 109, "Atal'alarion",
                                  -480.4f, 96.6f, -189.7f, 30.0f, 0, 0, /*eventId*/ 11),
                    // Start the Avatar encounter: USE the Egg of Hakkar at the
                    // CENTRE of the Sanctum of the Fallen (the Shade-spawn spot),
                    // not down at the pit idol (a QUESTGIVER no-op). Anchored at
                    // the room centre so boss-nav stands the tank there before the
                    // egg fires (event 8). Same spot as the Avatar anchor below.
                    MakeObjective(OBJ(9), 17, 109, "Awaken the Soulflayer",
                                  -466.8f, 272.9f, -90.4f, 8.0f, 0, 0, /*eventId*/ 8),
                    // Avatar manifests in the north flame room (event 9). NO
                    // gateEntry: a gate firing the Persistent event from afar (the
                    // tank still in the pit) would run its KillCreature gates with
                    // the channelers/Avatar beyond the 80yd search and falsely
                    // complete the fight. arriveRadius-only makes boss-nav travel
                    // the tank INTO the room before the event starts.
                    MakeObjective(OBJ(10), 18, 109, "Avatar of Hakkar",
                                  -466.8f, 272.9f, -90.4f, 15.0f, /*gateEntry*/ 0,
                                  0, /*eventId*/ 9),
                };
                t.push_back(std::move(p));
            }

            // --- Razorfen Downs (map 129) --------------------------------
            // Tuten'kash is the first encounter (DungeonEncounter bit 0) but he has
            // NO static creature spawn — he is summoned only after the party rings
            // the entrance gong three times (see the "Ring the Gong" conditional
            // event + condition 4). With no spawn, BossSpawnIndex never emits him,
            // so without this he would never head the list, be travelled to, or be
            // tracked. Add him with his real encounterIndex 0 (default) so his kill
            // flips bit 0 the moment he spawns.
            //
            // The anchor sits ON THE GONG (148917 @ 2552,857), NOT his summon spot.
            // This is deliberate: it makes the boss navigation drive the tank to
            // the gong exactly as to any boss — long-range pathfinder, dynamic-pull
            // camps, combat — which is the robust travel an event step cannot do.
            // The gong event then rings in place. While he is absent the tank
            // arrives at the gong and the event (relevance 31) rings, preempting the
            // boss-not-present stall (relevance 20); once the third ring summons him
            // at his real spot (~80yd off) live-boss tracking retargets the engage
            // to his actual position.
            {
                BossRosterPatch p;
                p.mapId = 129;
                p.add = {
                    MakeBoss(7355, 129, "Tuten'kash",
                             /*on the gong*/ 2552.44f, 856.984f, 51.495f, /*completionFrom*/ 0),
                };
                t.push_back(std::move(p));
            }

            // --- ZulFarrak (map 209) -------------------------------------
            // ORDER FIX. The DBC encounter order does NOT match the travel path:
            // Hydromancer Velratha carries bit 0, so the auto path sent the tank
            // straight to the far sacred pool FIRST, before anything else. The
            // sensible (and Blizzard-intended) clear sweeps the dungeon front to
            // back and saves the pool for last:
            //   1. Theka the Martyr      (DBC bit 3)
            //   2. Antu'sul              (DBC bit 2)
            //   3. Witch Doctor Zum'rah  (DBC bit 4)
            //   4. Temple Summit         (objective — the Executioner event)
            //   5. Chief Ukorz Sandscalp (DBC bit 7 — opens after the event)
            //   6. Hydromancer Velratha  (DBC bit 0 — at the sacred pool)
            //   7. Sacred Pool           (objective — the Gahz'rilla gong)
            // Reorder the five auto-derived bosses in place (orderOverride keys
            // 1..6 on a contiguous scale shared with the two objectives) so each
            // sorts into its path slot while its real DBC kill-bit is untouched.
            //
            // The temple event (Executioner / Bly's Band) at the top of the great
            // staircase opens the door to Chief Ukorz; the tank must trigger it
            // before heading to Ukorz or it bee-lines into his closed door and
            // stalls. The Temple Summit objective (key 4) sits just before Ukorz
            // (key 5). eventId 1 is the full PERSISTENT temple step list (see
            // ZulFarrakEvents.cpp): kill the executioner, crack a cage, survive
            // the waves, descend to help kill the temple bosses, gossip Weegli
            // (door) and Bly (final fight). Only when Bly dies does the event
            // complete and latch this objective, after which the clear proceeds
            // to Ukorz with the door open.
            {
                BossRosterPatch p;
                p.mapId = 209;
                p.reorder = {
                    { 7272, 1 },  // Theka the Martyr      (DBC bit 3)
                    { 8127, 2 },  // Antu'sul              (DBC bit 2)
                    { 7271, 3 },  // Witch Doctor Zum'rah  (DBC bit 4)
                    { 7267, 5 },  // Chief Ukorz Sandscalp (DBC bit 7)
                    { 7795, 6 },  // Hydromancer Velratha  (DBC bit 0)
                };
                p.add = {
                    MakeObjective(OBJ(1), /*encounterIndex*/ 7, 209,
                                  "Temple Summit (Executioner event)",
                                  1886.8f, 1289.9f, 46.0f,
                                  /*arriveRadius*/ 12.0f, /*gateEntry*/ 0,
                                  /*hook*/ 0, /*eventId*/ 1, /*orderOverride*/ 4),
                    // The optional Gahz'rilla gong, ordered LAST (key 7): the
                    // strictly-ordinal picker only routes the tank to the sacred
                    // pool once every real boss is dead. Anchor sits ON the gong
                    // (GO 141832 spawn coords) so boss-nav delivers the tank right
                    // to it; eventId 2 (see ZulFarrakEvents.cpp) rings it and kills
                    // the summoned boss. gateEntry 0: the event owns completion
                    // (Gahz'rilla dead), not "boss alive". The objective carries no
                    // real kill-bit, so its key can't collide with a set encounter
                    // bit.
                    MakeObjective(OBJ(2), /*encounterIndex*/ 8, 209,
                                  "Sacred Pool (Gahz'rilla gong)",
                                  1650.91f, 1171.88f, 10.901f,
                                  /*arriveRadius*/ 12.0f, /*gateEntry*/ 0,
                                  /*hook*/ 0, /*eventId*/ 2, /*orderOverride*/ 7),
                };
                t.push_back(std::move(p));
            }

            // --- Blackrock Depths (map 230) — Ring of Law arena gauntlet --
            // The Ring of Law is its OWN DungeonEncounter (bit 3, between
            // Houndmaster Grebmar at 2 and Pyromancer Loregrain at 4), credited
            // to Grimstone (10096) — but Grimstone has NO static spawn (he is
            // summoned only when the centre area trigger fires), so BossSpawnIndex
            // can't emit him, exactly like Razorfen Downs' Tuten'kash. Add the
            // Ring of Law as an OBJECTIVE anchor at the arena centre (area trigger
            // 1526, x/y from AreaTrigger.dbc; floor z) so boss-nav drives the tank
            // into the sealed arena and the event (eventId 1) runs the gauntlet:
            // walk in -> the trigger fires -> hold dead-centre until DONE while
            // the random waves + boss are fought reactively. encounterIndex 3
            // slots it after Grebmar and before Loregrain; the objective-before-
            // boss tie-break + the picker's strictly-greater advance order it
            // correctly. No gateEntry (the boss is random; the event owns
            // completion via TYPE_RING_OF_LAW == DONE, see BlackrockDepthsEvents).
            {
                BossRosterPatch p;
                p.mapId = 230;
                p.add = {
                    MakeObjective(OBJ(1), /*encounterIndex*/ 3, 230, "Ring of Law",
                                  596.432f, -188.498f, -53.9f, /*arriveRadius*/ 12.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
                };
                t.push_back(std::move(p));
            }

            // --- The Deadmines (map 36) — Defias Cannon door ------------------
            // The Iron Clad Door (GO 16397, lock 202 — rogue-pick only) seals the
            // foundry from the pirate ship; a bot party can never click it open.
            // Add the Defias Cannon (GO 16398) as an OBJECTIVE anchor so boss-nav
            // drives the tank to it between Gilnid (bit 2, last foundry boss) and
            // Mr. Smite (bit 3, first ship boss); the event (eventId 1) fires the
            // cannon, opening the door to the whole ship (Smite 3, Cookie 4,
            // Greenskin 5, VanCleef 6). encounterIndex 3 is shared with Mr. Smite:
            // the objective-before-boss tie-break + the picker's strictly-greater
            // advance hand the objective over first, then the boss. No gateEntry
            // (the event owns completion via the door-open gate; see
            // DeadminesEvents).
            {
                BossRosterPatch p;
                p.mapId = 36;
                p.add = {
                    MakeObjective(OBJ(1), /*encounterIndex*/ 3, 36, "Iron Clad Door",
                                  -107.56f, -659.67f, 7.21f, /*arriveRadius*/ 10.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
                };
                t.push_back(std::move(p));
            }

            // --- Wailing Caverns (map 43) — drop down to Lord Serpentis -------
            // Lord Serpentis (3673, encounterIndex 5) sits on an upper-ground
            // navmesh island the party reaches only by running to a ledge and
            // DROPPING onto it; Recast bakes no off-mesh link across the gap, so
            // stock boss-nav calls him unreachable. Add an OBJECTIVE anchor at the
            // LIP (approach-side mesh -> reachable) sharing Serpentis's bit 5; the
            // objective-before-boss tie-break drives the tank there first, the
            // event (eventId 1) jumps the gap, then the clear advances to
            // Serpentis (now on the same island). No gateEntry: the event owns
            // completion via the Jump landing. See WailingCavernsEvents.
            {
                BossRosterPatch p;
                p.mapId = 43;
                p.add = {
                    MakeObjective(OBJ(1), /*encounterIndex*/ 5, 43, "Drop to Lord Serpentis",
                                  -290.65567f, -3.8297224f, -58.30473f, /*arriveRadius*/ 6.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
                };
                t.push_back(std::move(p));
            }

            // --- Stratholme (map 329) — the Slaughterhouse / Baron run --------
            // Ramstein the Gorger (DBC bit 11) is SUMMONED once the Slaughter
            // Square abominations die — he has no creature.sql spawn, so
            // BossSpawnIndex (which walks spawns) never lists him and the tank
            // can't anchor on him. Add ONE objective at the Slaughter Square
            // (SlaughterPos from instance_stratholme.cpp), ordered at Ramstein's
            // bit 11 (after the ziggurats 7/8/9 + Barthilas 10, before Baron 12),
            // carrying the persistent slaughter event (eventId 4): clear the
            // abominations -> Ramstein -> 33 Mindless Undead -> 5 Black Guards,
            // which opens Baron's door. The event's KillCreatureEngage(Ramstein)
            // flips his real bit when he dies (objectives skip the completion-mask
            // check, so the orderIndex is a pure ordering hint that can't collide).
            //
            // The three ziggurat bosses and Baron need NO roster change — they
            // have static spawns and real bits and are pulled normally; their
            // in-ziggurat acolyte follow-ups are conditional events (5/6/7).
            //
            // ORDER FIX. The DBC bits put the ziggurats (Baroness 7, Nerub'enkan
            // 8, Maleki 9) BEFORE Magistrate Barthilas (10), but the dead-side
            // path runs Barthilas FIRST (he flees the entrance to warn the Baron),
            // then the ziggurats, then the slaughterhouse, then Baron. So re-add
            // Barthilas with orderOverride 6 — just after the live side (Balnazzar
            // 6) and before Baroness 7 — while keeping his real kill-bit 10
            // (completionFrom = his own entry). Net dead-side order: Barthilas ->
            // ziggurats 7/8/9 -> Slaughterhouse 11 -> Baron 12.
            //
            // Both the live (Scarlet) and dead (Undead) sides stay in the list so
            // one run does both; only Barthilas's slot moves. Barthilas's spawn
            // coords are his static creature.sql spawn (he relocates at run-time,
            // but the auto-list anchored on this spawn and the engage pipeline
            // handles his flee). See StratholmeEvents.
            //
            // LIVE-SIDE BALNAZZAR. The Balnazzar encounter (DBC bit 6) credits
            // creature 10813 (Balnazzar), which has NO creature.sql spawn — it
            // exists only via Grand Crusader Dathrohan's (10812) on-aggro SmartAI,
            // which at <=40% HP casts Balnazzar Transform and UpdateEntry's the
            // SAME creature 10812 -> 10813. So BossSpawnIndex (which matches credit
            // entries to spawns) drops the encounter and the clear jumps straight
            // from Archivist Galford (bit 5) to the dead side — the live boss is
            // never fought. Same class as Ramstein. Fix: an OBJECTIVE at Dathrohan's
            // static spawn ordered at bit 6 (right after Galford, sharing key 6 with
            // the reordered Barthilas — Apply's Objective-before-Boss tie-break +
            // PickTarget's equal-index loop run Dathrohan then Barthilas), carrying
            // event 5 (KillCreatureEngage 10812 -> 10813). Completion is the event
            // ("Balnazzar dead"), not the mask, so objectives' bit-6 ordering hint
            // can't collide with the real bit-6 the encounter sets. See
            // StratholmeEvents.
            {
                BossRosterPatch p;
                p.mapId = 329;
                p.remove = { 10435 };  // Barthilas — re-added below, reordered
                p.add = {
                    MakeBoss(10435, 329, "Magistrate Barthilas",
                             3663.23f, -3619.14f, 137.98f,
                             /*completionFrom*/ 10435, /*orderOverride*/ 6),
                    // Grand Crusader Dathrohan -> Balnazzar (live side). Objective
                    // at his static spawn (creature.sql, map 329), bit 6 so it slots
                    // right after Galford; event 5 seeks + engages him and finishes
                    // the transformed Balnazzar. See the LIVE-SIDE BALNAZZAR note.
                    MakeObjective(OBJ(2), /*encounterIndex*/ 6, 329,
                                  "Grand Crusader Dathrohan",
                                  3415.8f, -3044.5f, 136.8f, /*arriveRadius*/ 30.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 5),
                    // Anchor pulled ~37yd SOUTH of SlaughterPos into the abomination
                    // hall: SlaughterPos (4032,-3378) sits at the still-closed Baron
                    // door (175796 @ -3364), so the approach hit the door and stalled
                    // ("closed door blocking path"). This spot is in the open hall,
                    // reachable from the south entrance gate without crossing a closed
                    // door; the event's ClearRadius (r70) covers the whole hall.
                    MakeObjective(OBJ(1), /*encounterIndex*/ 11, 329,
                                  "Slaughterhouse (Baron run)",
                                  4032.0f, -3415.0f, 118.0f, /*arriveRadius*/ 30.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 4),
                };
                t.push_back(std::move(p));
            }

            // --- Dire Maul (map 429) — East + West (ONE patch per map) ----
            // FindPatch returns the FIRST patch matching a map, so all of Dire
            // Maul's wings share a SINGLE map-429 BossRosterPatch. Wing-filtering
            // (DungeonWingRegistry, applied after this) scopes each run to its
            // wing, so the East objective, the West reorder and the West pylon
            // objectives coexist here harmlessly — an East run never sees the
            // West adds and vice versa. orderOverride integers may therefore
            // repeat across wings (ordering is computed per wing).
            //
            // EAST — Conservatory Door objective. Alzzin's grove is sealed behind
            // the Conservatory Door (GO 176907), opened only by Ironbark the
            // Redeemed (14241) after a gossip. A travel OBJECTIVE at Ironbark's
            // spawn (wired to event 429/1) lets boss-nav deliver the tank to him;
            // the anchored event gossips him and waits for the door he walks off
            // to open. ANCHORED because the opener stands ~190yd OFF the corridor
            // (contrast the North Gordok / West Crescent doors, which sit ON the
            // corridor and so are CONDITIONAL — see DireMaulEvents.cpp). East
            // bosses reordered onto 10/20/30/50 with a gap (40) for the objective.
            //
            // WEST — order fix + Immol'thar pylons. The DBC encounter order does
            // not match the kill order, and Prince Tortheldrin (11486) must be
            // last: he is friendly/non-attackable until Immol'thar (11496) dies,
            // yet his chamber (y625) sits BEFORE Immol'thar (y812) on the walk.
            // Reorder so Immol'thar precedes the Prince. Immol'thar is held
            // NON_ATTACKABLE by a force field (GO 179503) until all five Crystal
            // Generators (pylons) are destroyed (the instance ORs a bitmask and
            // clears the flag at 0x1F). The five generators (BUTTON GOs) are
            // scattered wing-wide, far beyond HopTo range, so each is a travel
            // OBJECTIVE wired to a tiny anchored UseGO event (DireMaulEvents
            // 429/4..429/8) — the Sunken-Temple / Uldaman-altar pattern. Ordered
            // south-trio-before-Tendris / north-pair-before-Immol'thar to track
            // the route (pure orderOverride tuning if a backtrack looks bad live).
            // Engage of Immol'thar naturally waits on his NON_ATTACKABLE flag, so
            // no explicit "all pylons down" gate is needed. The objective
            // encounterIndex values are synthetic highs (40 East; 41-45 West
            // pylons) distinct from any real DBC kill-bit; objective completion
            // keys on the anchor latch (eventId), never on encounterIndex.
            //
            // All synthetic objective entries are added to their wing's list in
            // DungeonWingRegistry so wing-filtering keeps them.
            {
                BossRosterPatch p;
                p.mapId = 429;
                p.reorder = {
                    // East
                    { 11490, 10 },  // Zevrim Thornhoof
                    { 13280, 20 },  // Hydrospawn
                    { 14327, 30 },  // Lethtendris
                    { 11492, 50 },  // Alzzin the Wildshaper (last)
                    // West
                    { 11489, 10 },  // Tendris Warpwood
                    { 11488, 20 },  // Illyanna Ravenoak
                    { 11487, 30 },  // Magister Kalendris
                    { 11496, 45 },  // Immol'thar (after all five pylons)
                    { 11486, 50 },  // Prince Tortheldrin (last)
                };
                p.add = {
                    // East — Ironbark / Conservatory Door.
                    MakeObjective(OBJ(1), /*encounterIndex*/ 40, 429,
                                  "Ironbark the Redeemed (Conservatory Door)",
                                  -56.59f, -269.12f, -57.87f,
                                  /*arriveRadius*/ 12.0f, /*gateEntry*/ 0,
                                  /*hook*/ 0, /*eventId*/ 1, /*orderOverride*/ 40),
                    // West — Immol'thar pylons. Southern trio (before Tendris).
                    MakeObjective(OBJ(2), /*encounterIndex*/ 41, 429,
                                  "Destroy Demon Crystal (Generator 1)",
                                  12.94f, 277.93f, -8.93f,
                                  /*arriveRadius*/ 10.0f, /*gateEntry*/ 0,
                                  /*hook*/ 0, /*eventId*/ 4, /*orderOverride*/ 5),
                    MakeObjective(OBJ(3), /*encounterIndex*/ 42, 429,
                                  "Destroy Demon Crystal (Generator 2)",
                                  -92.35f, 442.67f, 28.55f,
                                  10.0f, 0, 0, /*eventId*/ 5, /*orderOverride*/ 6),
                    MakeObjective(OBJ(4), /*encounterIndex*/ 43, 429,
                                  "Destroy Demon Crystal (Generator 3)",
                                  121.22f, 429.09f, 28.45f,
                                  10.0f, 0, 0, /*eventId*/ 6, /*orderOverride*/ 7),
                    // West — northern pair (by Immol'thar's prison, after Kalendris).
                    MakeObjective(OBJ(5), /*encounterIndex*/ 44, 429,
                                  "Destroy Demon Crystal (Generator 4)",
                                  78.14f, 737.40f, -24.62f,
                                  10.0f, 0, 0, /*eventId*/ 7, /*orderOverride*/ 40),
                    MakeObjective(OBJ(6), /*encounterIndex*/ 45, 429,
                                  "Destroy Demon Crystal (Generator 5)",
                                  -155.43f, 734.17f, -24.62f,
                                  10.0f, 0, 0, /*eventId*/ 8, /*orderOverride*/ 41),
                };
                t.push_back(std::move(p));
            }

            // --- Uldaman (map 70) — Altar of the Keepers + Altar of Archaedas ---
            // The path to Archaedas runs through two GAMEOBJECT summoning rituals:
            // the Altar of the Keepers (wake + kill the 4 Stone Keepers to open the
            // temple door) and the Altar of Archaedas (wake the stoned final boss).
            // Both altars are FAR from any conditional gate and surrounded only by
            // dormant, immune statues — so a conditional event (HopTo-only, no
            // long-range nav) fires the instant the tank nears the room and then
            // can't reach the altar (the live "never clicks the altar" bug). Make
            // them travel OBJECTIVES so the boss-nav LongRangePathfinder delivers
            // the tank ONTO each altar first; the anchored events (UldamanEvents
            // 70/2 and 70/3) then fire the rituals. Mirrors Dire Maul's Ironbark
            // and Stratholme's Slaughterhouse.
            //
            // Encounter order is Grimlok (bit 6) -> Archaedas (bit 7) with no
            // integer slot between them, so bump Archaedas's clear ORDER to 10
            // (his real DBC kill-bit 7 is untouched — completion keys on
            // encounterIndex, never on the order override) and slot the keeper
            // altar at 8, the Archaedas altar at 9. The keeper altar sits a full
            // floor ABOVE Archaedas (z -26.5 vs -51.7) at nearly the same x/y, so
            // visiting it as an explicit waypoint also stops the boss-nav from
            // dragging the tank straight down to the stoned boss below.
            {
                BossRosterPatch p;
                p.mapId = 70;
                p.reorder = { { 2748, 10 } };  // Archaedas — order only, bit 7 kept
                p.add = {
                    // arriveRadius 30 so the tank "arrives" on reaching the hall
                    // and the event's ClearRadius (r40) drives the trash clear from
                    // the entrance inward, rather than shoving to the altar first.
                    MakeObjective(OBJ(1), /*encounterIndex*/ 7, 70,
                                  "Altar of the Keepers",
                                  104.85f, 272.45f, -26.53f, /*arriveRadius*/ 30.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 2,
                                  /*orderOverride*/ 8),
                    MakeObjective(OBJ(2), /*encounterIndex*/ 7, 70,
                                  "Altar of Archaedas",
                                  96.48f, 269.05f, -52.15f, /*arriveRadius*/ 10.0f,
                                  /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 3,
                                  /*orderOverride*/ 9),
                };
                t.push_back(std::move(p));
            }

            return t;
        }();
        return kPatches;
    }

    BossRosterPatch const* FindPatch(uint32 mapId)
    {
        for (BossRosterPatch const& p : PatchTable())
            if (p.mapId == mapId)
                return &p;
        return nullptr;
    }
}

bool BossRosterRegistry::HasPatch(uint32 mapId)
{
    return FindPatch(mapId) != nullptr;
}

std::vector<DungeonBossInfo> BossRosterRegistry::Apply(uint32 mapId, std::vector<DungeonBossInfo> base)
{
    BossRosterPatch const* patch = FindPatch(mapId);
    if (!patch)
        return base;

    // 1. Resolve inherited completion bits from the (still present) base list
    //    before anything is removed, then drop the removed entries.
    std::vector<DungeonBossInfo> adds = patch->add;
    for (DungeonBossInfo& a : adds)
    {
        if (a.inheritCompletionFrom)
        {
            for (DungeonBossInfo const& b : base)
            {
                if (b.entry == a.inheritCompletionFrom)
                {
                    a.encounterIndex = b.encounterIndex;
                    break;
                }
            }
            a.inheritCompletionFrom = 0;
        }
    }

    std::unordered_set<uint32> const remove(patch->remove.begin(), patch->remove.end());
    std::vector<DungeonBossInfo> result;
    result.reserve(base.size() + adds.size());
    for (DungeonBossInfo const& b : base)
        if (!remove.count(b.entry))
            result.push_back(b);

    // 1b. Reorder auto-derived bosses in place: stamp the requested
    //     orderOverride onto each kept entry so it sorts by its clear-path key
    //     while its real DBC kill-bit (encounterIndex) stays as-is. Lets a
    //     dungeon whose DBC encounter order doesn't match the travel path be
    //     fixed without remove+re-add (cf. ZulFarrak: Velratha's DBC bit 0
    //     would otherwise send the tank to the far sacred pool first).
    for (auto const& [entry, order] : patch->reorder)
        for (DungeonBossInfo& b : result)
            if (b.entry == entry)
            {
                b.orderOverride = order;
                break;
            }

    // 2. Append the added anchors and re-sort into clear order so objectives
    //    and replacement bosses slot in. Ordering keys on BossOrderKey (the
    //    explicit orderOverride when set, else encounterIndex) so a reordered
    //    real boss lands in its path slot without disturbing its kill-bit.
    for (DungeonBossInfo& a : adds)
        result.push_back(std::move(a));

    // Sort by order key. Tie-break: at an equal key an Objective sorts BEFORE a
    // Boss, so an objective sharing a boss's bit is reached first. The candidate
    // picker (NextDungeonBossValue::PickTarget) advances to the lowest key
    // STRICTLY greater than the one just finished, so an objective tied with the
    // boss it must precede (e.g. ZulFarrak's summit event at bit 7, the same bit
    // as Chief Ukorz) would otherwise be skipped past; ordering it first makes
    // the picker hand it over before the boss.
    std::stable_sort(result.begin(), result.end(),
                     [](DungeonBossInfo const& l, DungeonBossInfo const& r)
                     {
                         uint32 const lk = BossOrderKey(l);
                         uint32 const rk = BossOrderKey(r);
                         if (lk != rk)
                             return lk < rk;
                         return l.kind == DungeonAnchorKind::Objective &&
                                r.kind == DungeonAnchorKind::Boss;
                     });

    return result;
}
