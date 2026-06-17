/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- Wailing Caverns (map 43) — the DROP to LORD SERPENTIS, ANCHORED + PERSISTENT ---
// Lord Serpentis (3673) sits on an upper-ground navmesh component that the party
// reaches only by running to a ledge and DROPPING DOWN onto it. Recast bakes no
// off-mesh connection across that vertical gap (the island dumper shows the
// Serpentis shelf as a disconnected component), so stock boss-nav declares him
// unreachable and the clear stalls at the lip.
//
// Navigation is by the boss list: an OBJECTIVE anchor is added at the LIP
// (BossRosterRegistry, encounterIndex 5 — shared with Serpentis; the
// objective-before-boss tie-break runs it first). Boss-nav drives the tank to
// the lip exactly as to any boss — the lip is on the approach-side mesh, so it
// IS reachable — and the event then jumps the one off-mesh leg. Once landed the
// bot is on Serpentis's island and stock nav walks it the rest of the way; the
// objective latches and the clear advances to Serpentis (bit 5) and Verdan
// (bit 6).
//
// Coords are from in-game observation (a GM stood at the jump): the lip is the
// edge the player leaps from, the landing a few yards onto the lower shelf
// (~5.6yd drop, ~9.4yd across — a clean run-speed MoveJump).
//
// PERSISTENT because the drop is ONE-WAY: the bot cannot path back up across the
// gap. A non-persistent anchored event REWINDS to step 0 after any >1s Drive gap
// (e.g. the bot lands among trash and the combat engine takes over for a few
// seconds); that rewind would re-run MoveTo(lip) and walk the bot BACK toward
// the now-unreachable lip. Persistence keeps the step progress across the gap so
// the jump fires exactly once and never regresses. The Jump step is itself
// idempotent (Done once within radius of the landing), a second guard against
// re-firing.

void RegisterWailingCavernsEvents(std::vector<DungeonEvent>& out)
{
    // The ledge the party leaps FROM (approach-side mesh, where the objective
    // anchor sits) and the shelf it lands ON (Serpentis's island).
    constexpr float WC_LIP_X  = -290.65567f;
    constexpr float WC_LIP_Y  = -3.8297224f;
    constexpr float WC_LIP_Z  = -58.30473f;
    constexpr float WC_LAND_X = -285.45773f;
    constexpr float WC_LAND_Y = 4.021016f;
    constexpr float WC_LAND_Z = -63.919395f;

    out.push_back(
        EventBuilder(43, 1, "Drop to Lord Serpentis")
            .Anchored(/*encounterIndex*/ 5)
            .Persistent()
            // 1. Settle exactly on the lip (the objective's arrive radius may
            //    leave the tank a few yards off — too far off and the leap
            //    misses the shelf).
            .MoveTo(WC_LIP_X, WC_LIP_Y, WC_LIP_Z, /*radius*/ 3.0f)
            // 2. Leap the off-mesh gap onto Serpentis's shelf. Done once landed;
            //    stock boss-nav then carries the bot to Serpentis.
            .Jump(WC_LAND_X, WC_LAND_Y, WC_LAND_Z, /*radius*/ 5.0f)
            .Build());
}
