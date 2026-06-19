/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- Dire Maul East (map 429) — Ironbark / Conservatory Door -------------
// Alzzin the Wildshaper's grove is sealed behind the Conservatory Door (GO
// 176907), which the party cannot open. Ironbark the Redeemed (14241), who
// stands at the west entrance corridor, opens it: pick his single gossip option
// (menu 5602, option 0) and his SmartAI walks him east along a 14-point path to
// the door and, ~7s after arriving, swings it open (and sets instance data
// TYPE_EAST_WING_PROGRESS=2), then despawns. Total gossip->open ≈ 20-30s.
//
// Anchored (not Conditional like Shadowfang Keep's twin courtyard door): when
// this becomes relevant the tank is ~190 yds away in the southern boss chamber
// (Zevrim/Hydrospawn/Lethtendris are reached WITHOUT the door; only Alzzin sits
// behind it). A Conditional Gossip step has no long-range navigation — it only
// HopTo's (raw MovePoint, 74-node capped) — which can't reliably cross that gap.
// An anchored OBJECTIVE gets the boss-nav LongRangePathfinder to deliver the
// tank to Ironbark first (BossRosterRegistry slots the objective at his spawn,
// ordered just before Alzzin); then this gossips him and waits for the door.
//
// No MoveTo to the door is needed: while WaitForGOState holds, the tank stays at
// Ironbark's anchor (west) and Ironbark walks east and opens the door himself.
// Holding the run here (relevance 31) also keeps the DC door machinery from
// prematurely force-opening the lock-free Conservatory Door (which would bypass
// Ironbark) or auto-pausing on the door-blocked timeout before the ~25s open. On
// completion boss-nav routes the tank through the now-open door to Alzzin.
//
// Persistent so a stray combat tick-gap can't rewind to step 0 and re-gossip —
// Ironbark removes his gossip npcflag on the first select, so a re-talk would
// find an empty menu and stall. SkipIfTargetMissing lets the gossip step skip
// once he has walked off / despawned; Optional degrades a non-firing script to
// the normal door-blocked stall (the door still opens, driven by his own AI).

void RegisterDireMaulEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(429, 1, "Ironbark opens the Conservatory Door")
                      .Anchored(/*orderIndex, doc-only*/ 4)
                      .Gossip(/*Ironbark the Redeemed*/ 14241, /*option*/ 0,
                              /*searchRadius*/ 20.0f)
                          .SkipIfTargetMissing()
                      // Ironbark's SmartAI opens the door with SetGoState(2) =
                      // GO_STATE_ACTIVE_ALTERNATIVE (not the usual ACTIVE=0), so
                      // wait for 2. Wide search: the tank holds at Ironbark's
                      // spawn while the door is ~187 yds east down the corridor,
                      // far beyond the 20yd GO-search default — without this the
                      // step never finds the GO and holds until timeout.
                      .WaitForGOState(/*Conservatory Door*/ 176907,
                                      /*GO_STATE_ACTIVE_ALTERNATIVE*/ 2,
                                      /*timeout*/ 90000, /*searchRadius*/ 250.0f)
                      .Persistent()
                      .Optional()
                      .Build());
}
