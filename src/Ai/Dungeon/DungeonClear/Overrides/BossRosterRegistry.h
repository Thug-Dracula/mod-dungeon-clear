/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_BOSSROSTERREGISTRY_H
#define _PLAYERBOT_BOSSROSTERREGISTRY_H

#include <vector>

#include "Common.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"

// Per-dungeon corrections to the auto-derived boss list (BossSpawnIndex).
//
// Many dungeons spawn a boss only after an event, gate a boss behind an event
// the party must travel through, or simply carry the wrong creature in their
// DungeonEncounter.dbc data. This registry lets each such dungeon express a
// small PATCH against the derived list — remove the wrong/locked entries, add
// the real boss (with hand-authored navmesh coords), and/or add travel
// "objective" anchors the tank leads the party to.
//
// Authoring is one row per dungeon in a hardcoded table (see
// BossRosterRegistry.cpp), mirroring RoomAggroRegistry. This is link-safe in
// the static-lib build (a hardcoded table in a GLOBed .cpp), unlike
// self-registering static initializers which the linker strips.
struct BossRosterPatch
{
    uint32 mapId{0};
    // Entries to drop from the auto-derived list (wrong / event-locked bosses).
    std::vector<uint32> remove;
    // Bosses / objectives to add. Coords are hand-authored; objectives use
    // DungeonAnchorKind::Objective. A boss with inheritCompletionFrom set
    // borrows that (to-be-removed) entry's encounterIndex for its kill-bit.
    std::vector<DungeonBossInfo> add;
};

class BossRosterRegistry
{
public:
    // Synthetic entry for the seq-th non-creature objective anchor on a map.
    // Real creature entries never reach this range, so an objective gets a
    // unique nonzero entry that flows through the entry-keyed machinery (skip /
    // sticky / cleared-anchor latch / panel) without colliding with a spawn.
    // Exposed so an event file can name the objective it sorts relative to
    // in the `dc bosses` panel (DungeonEvent::panelGatesBossEntry).
    static constexpr uint32 ObjectiveEntry(uint32 seq) { return 0x4F000000u | seq; }

    // Returns the patched boss list for `mapId`. If no patch is registered the
    // base list is returned unchanged. Patches are difficulty-agnostic (5-man
    // rosters match across normal/heroic), so the lookup keys on mapId only.
    static std::vector<DungeonBossInfo> Apply(uint32 mapId, std::vector<DungeonBossInfo> base);

    // True if any patch exists for the map (cheap gate for callers).
    static bool HasPatch(uint32 mapId);
};

#endif
