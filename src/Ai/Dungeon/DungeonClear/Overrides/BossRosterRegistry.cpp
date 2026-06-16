/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BossRosterRegistry.h"

#include <algorithm>
#include <unordered_set>

namespace
{
    // Synthetic entry base for non-creature objectives. Real creature entries
    // never reach this range, so objective anchors get a unique nonzero entry
    // that flows through the existing entry-keyed machinery (skip / sticky /
    // cleared-anchor latch) without colliding with any spawn.
    constexpr uint32 OBJECTIVE_ENTRY_BASE = 0x4F000000u;
    constexpr uint32 OBJ(uint32 seq) { return OBJECTIVE_ENTRY_BASE | seq; }

    // Build a boss anchor with an inherited completion bit. `completionFrom` is
    // the auto-derived entry whose encounterIndex (kill-bit) this boss borrows
    // — used when the real boss has no DungeonEncounter row of its own.
    DungeonBossInfo MakeBoss(uint32 entry, uint32 mapId, char const* name,
                             float x, float y, float z, uint32 completionFrom)
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
        return b;
    }

    // Build a travel-objective anchor. `orderIndex` slots it into the encounter
    // ordering; `gateEntry` (optional) is a creature whose live presence also
    // satisfies the objective; `hook` (optional) is an ObjectiveHookRegistry id.
    DungeonBossInfo MakeObjective(uint32 entry, uint32 encounterIndex, uint32 mapId,
                                  char const* name, float x, float y, float z,
                                  float arriveRadius = 0.0f, uint32 gateEntry = 0,
                                  uint32 hook = 0, uint32 eventId = 0)
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
            {
                BossRosterPatch p;
                p.mapId = 189;
                p.remove = { 3977 };
                p.add = {
                    MakeBoss(3976, 189, "Scarlet Commander Mograine",
                             1153.9f, 1398.4f, 32.6f, /*completionFrom*/ 3977),
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
            // The temple (Executioner / Bly's Band) event runs at the top of the
            // great staircase, and completing it opens the door to the final boss
            // Chief Ukorz Sandscalp (bit 7). Without a target up there the tank
            // heads straight for Ukorz, hits his closed door, and stalls. Add a
            // travel objective at the Sandfury Executioner's spot (top of the
            // stairs, verified from the creature table) ordered at bit 7 — the
            // equal-index tie-break sorts an objective BEFORE the boss it shares an
            // index with, so the tank goes up and triggers the event first.
            // eventId 1 is the full PERSISTENT temple step list (see
            // ZulFarrakEvents.cpp): the tank kills the executioner, cracks a cage,
            // survives the waves, descends to help kill the temple bosses, then
            // gossips Weegli (door) and Bly (final fight). Only when Bly dies does
            // the event complete and latch this objective, after which the clear
            // proceeds to Ukorz with the door open.
            {
                BossRosterPatch p;
                p.mapId = 209;
                p.add = {
                    MakeObjective(OBJ(1), /*orderIndex*/ 7, 209,
                                  "Temple Summit (Executioner event)",
                                  1886.8f, 1289.9f, 46.0f,
                                  /*arriveRadius*/ 12.0f, /*gateEntry*/ 0,
                                  /*hook*/ 0, /*eventId*/ 1),
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

    // 2. Append the added anchors and re-sort by encounter index so objectives
    //    and replacement bosses slot into the clear order.
    for (DungeonBossInfo& a : adds)
        result.push_back(std::move(a));

    // Sort by encounter index. Tie-break: at an equal index an Objective sorts
    // BEFORE a Boss, so an objective sharing a boss's bit is reached first. The
    // candidate picker (NextDungeonBossValue::PickTarget) advances to the
    // lowest index STRICTLY greater than the one just finished, so an objective
    // tied with the boss it must precede (e.g. ZulFarrak's summit event at bit 7,
    // the same bit as Chief Ukorz) would otherwise be skipped past; ordering it
    // first makes the picker hand it over before the boss.
    std::stable_sort(result.begin(), result.end(),
                     [](DungeonBossInfo const& l, DungeonBossInfo const& r)
                     {
                         if (l.encounterIndex != r.encounterIndex)
                             return l.encounterIndex < r.encounterIndex;
                         return l.kind == DungeonAnchorKind::Objective &&
                                r.kind == DungeonAnchorKind::Boss;
                     });

    return result;
}
