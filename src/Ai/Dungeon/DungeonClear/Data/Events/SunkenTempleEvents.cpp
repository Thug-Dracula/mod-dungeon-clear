/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- Sunken Temple (map 109) ---------------------------------------------

void RegisterSunkenTempleEvents(std::vector<DungeonEvent>& out)
{
    // Event 1 — the Altar of Hakkar objective: the tank leads the party to the
    // central altar (the anchor handles travel), then this event holds there
    // waiting for the Avatar of Hakkar (8443) to manifest from the Atal'ai
    // sacrifice. Optional: whether the summon triggers off the trash the bots
    // kill is unverified, so on timeout the event SKIPS (the clear advances)
    // rather than stalling forever — matching the roster note that you may have
    // to `dc skip` it. 45s tolerates a slow approach + the scripted sequence.
    out.push_back(EventBuilder(109, 1, "Altar of Hakkar (Avatar event)")
                      .Anchored(7)
                      .WaitForSpawn(8443, /*wantAlive*/ true, /*timeout*/ 45000)
                      .Optional()
                      .Build());
}
