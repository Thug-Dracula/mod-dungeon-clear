/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- ZulFarrak (map 209) -------------------------------------------------

void RegisterZulFarrakEvents(std::vector<DungeonEvent>& out)
{
    // Event 1 — Temple-summit (Sandfury prisoner) event: reaching the summit
    // fires the scripted event that opens Chief Ukorz's door. The anchor's
    // travel + arrival already drives that today; this event row migrates the
    // objective onto the framework with NO extra steps, so behaviour is
    // unchanged (arrive => complete). TODO(milestone 2): once the
    // prisoner-release lever / gossip is verified, add UseGO/Gossip +
    // WaitForGOState here so the door open is explicit rather than incidental.
    out.push_back(EventBuilder(209, 1, "Temple Summit (Executioner event)")
                      .Anchored(7)
                      .Build());
}
