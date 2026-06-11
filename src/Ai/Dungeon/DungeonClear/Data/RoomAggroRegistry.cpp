/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "RoomAggroRegistry.h"

#include <algorithm>

namespace
{
    // Tier-1 MVP scope (deployment-files/docs/mod-dungeon-clear_room-aggro-bosses
    // _plan.md). Radii are read straight from the script that force-pulls the
    // room, verified against the live source: SM Cathedral CATHEDRAL_PULL_RANGE
    // 80, Pandemonius ROOM_PULL_RANGE 70, Blackheart/Grand-Champions
    // CallForHelp(100), Dagran CallForHelp(VISIBLE_RANGE==166), and the SmartAI
    // CALL_FOR_HELP radii for Jammal'an (90), Pusillin (50), Krik'thir (40).
    //
    // memberEntries is left EMPTY (any hostile in radius) on purpose: the plan
    // endorses this over-approximation as the safe side (CallForHelp only wakes
    // assist-capable, non-evading friendlies, so clearing every hostile in the
    // radius is a strict superset), and it avoids hard-coding member entry IDs
    // we cannot verify per realm. The boss itself and any other encounter boss
    // are excluded at the use site (DcTargeting::IsDungeonBossEntry), and the
    // boss-aggro-sphere exclusion (IsRoomTrash) keeps the gate from livelocking
    // on a mob glued to the boss. A precise whitelist can be added later as a
    // pure optimisation without changing behaviour.
    RoomAggroBoss const kRoomAggroBosses[] =
    {
        // SM Cathedral (189): Mograine -> Whitemane. The grid AttackStart fires
        // on Mograine's engage; both encounter entries are flagged so the room
        // gate holds whichever the boss index selects.
        { 189, 3976, 80.0f, {} },   // Highlord Mograine
        { 189, 3977, 80.0f, {} },   // Inquisitor Whitemane

        { 555, 18667, 100.0f, {} }, // Shadow Labyrinth — Blackheart the Inciter
        { 109, 5710,   90.0f, {} }, // Sunken Temple — Jammal'an the Prophet
        { 557, 18341,  70.0f, {} }, // Mana-Tombs — Pandemonius
        { 601, 28684,  40.0f, {} }, // Azjol-Nerub — Krik'thir the Gatewatcher
        { 230, 9019,  166.0f, {} }, // Blackrock Depths — Emperor Dagran Thaurissan
        { 429, 14354,  50.0f, {} }, // Dire Maul East — Pusillin

        // Trial of the Champion (650) — Grand Champions, CallForHelp(100) on
        // unmount. Both faction rosters flagged; the other live champions are
        // themselves encounter bosses, so they fall out via the boss exclusion
        // and only genuine arena trash (if any) counts as the room.
        { 650, 34701, 100.0f, {} }, // Colosos
        { 650, 34702, 100.0f, {} }, // Jaelyne Evensong / Ambrose Boltspark
        { 650, 34703, 100.0f, {} }, // Lana Stouthammer / Mokra
        { 650, 34705, 100.0f, {} }, // Marshal Jacob Alerius
        { 650, 35569, 100.0f, {} }, // Eressea Dawnsinger
        { 650, 35570, 100.0f, {} }, // Zul'tore
        { 650, 35571, 100.0f, {} }, // Runok Wildmane
        { 650, 35572, 100.0f, {} }, // Mokra the Skullcrusher
        { 650, 35617, 100.0f, {} }, // Visceri
    };
}

RoomAggroBoss const* RoomAggroRegistry::Find(uint32 mapId, uint32 bossEntry)
{
    for (RoomAggroBoss const& b : kRoomAggroBosses)
        if (b.mapId == mapId && b.bossEntry == bossEntry)
            return &b;
    return nullptr;
}

bool RoomAggroRegistry::IsMemberEntry(RoomAggroBoss const& boss, uint32 entry)
{
    if (boss.memberEntries.empty())
        return true;
    return std::find(boss.memberEntries.begin(), boss.memberEntries.end(), entry) !=
           boss.memberEntries.end();
}

bool RoomAggroRegistry::IsRoomTrash(RoomAggroBoss const& boss, uint32 entry,
                                    float distToBoss, float bossSafeRadius)
{
    if (!IsMemberEntry(boss, entry))
        return false;
    if (distToBoss > boss.radius)
        return false;
    if (distToBoss <= bossSafeRadius)
        return false;
    return true;
}
