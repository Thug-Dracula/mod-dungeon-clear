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
    [[maybe_unused]] constexpr uint32 OBJ(uint32 seq) { return OBJECTIVE_ENTRY_BASE | seq; }

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
    [[maybe_unused]] DungeonBossInfo MakeObjective(uint32 entry, uint32 encounterIndex, uint32 mapId,
                                  char const* name, float x, float y, float z,
                                  float arriveRadius = 0.0f, uint32 gateEntry = 0,
                                  uint32 hook = 0)
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

            // --- TODO: Sunken Temple (map 109) ---------------------------
            // Bosses do not spawn until the party traverses the zone-wide
            // Atal'ai statue / Jammal'an event. Add travel objectives at the
            // event waypoints so the tank leads the party through it, e.g.:
            //
            //   BossRosterPatch p; p.mapId = 109;
            //   p.add = {
            //       MakeObjective(OBJ(1), /*orderIndex*/ 0, 109,
            //                     "Atal'ai Statue Circuit", X, Y, Z,
            //                     /*arriveRadius*/ 10.0f),
            //   };
            //
            // Coords intentionally deferred: they need live navmesh
            // verification (the statues sit around the central chamber rim).

            // --- TODO: ZulFarrak (map 209) -------------------------------
            // The pyramid (Gahz'rilla / Sergeant Bly prisoner) event triggers
            // when the party reaches the top of the temple steps. Add the
            // executioner / temple-top as an objective leading the tank up:
            //
            //   p.add = { MakeObjective(OBJ(1), idx, 209, "Temple Summit",
            //                           X, Y, Z, 12.0f, /*gateEntry*/ ...) };
            //
            // Coords deferred pending live verification.

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

    std::stable_sort(result.begin(), result.end(),
                     [](DungeonBossInfo const& l, DungeonBossInfo const& r)
                     { return l.encounterIndex < r.encounterIndex; });

    return result;
}
