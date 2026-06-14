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

            // --- Sunken Temple (map 109) — EXPERIMENTAL, needs live test -
            // The Avatar of Hakkar (8443) is summoned at the central Altar of
            // Hakkar only after the Atal'ai sacrifice event; it has NO creature
            // spawn, so BossSpawnIndex never emits it and the tank never goes to
            // the altar. Add a travel objective at the altar (GO "Altar of
            // Hakkar" coords) so the tank leads the party down into the central
            // pit, clearing the priest event en route. gateEntry 8443 resolves
            // the objective the moment the Avatar actually manifests. Ordered at
            // bit 7 — after Hazzas (bit 6), before Shade of Eranikus (bit 8).
            //
            // EXPERIMENTAL: whether the Avatar summon triggers off the trash the
            // bots kill (vs a scripted statue/quest gate) is unverified — if the
            // tank reaches the altar and nothing spawns, `dc skip` past it. The
            // statue-puzzle boss Atal'alarion is left in the auto-list as-is (it
            // has a real spawn) — the bot can't run that ordered-click puzzle, so
            // expect to `dc skip` it too.
            {
                BossRosterPatch p;
                p.mapId = 109;
                p.add = {
                    MakeObjective(OBJ(1), /*orderIndex*/ 7, 109,
                                  "Altar of Hakkar (Avatar event)",
                                  -420.8f, 94.7f, -174.2f,
                                  /*arriveRadius*/ 15.0f, /*gateEntry*/ 8443,
                                  /*hook*/ 0, /*eventId*/ 1),
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
