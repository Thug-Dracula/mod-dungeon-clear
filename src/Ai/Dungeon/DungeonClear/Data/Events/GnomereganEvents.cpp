/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "GameObject.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"

// --- Gnomeregan (map 90) — the WORKSHOP DOOR and THE FINAL CHAMBER ---------
//
// The stockade-style doors in Gnomeregan block progression. The Workshop Door
// (90858, lock 92) seals the tunnel between the entrance hub and the
// workshop/launch-bay area where Grubbis patrols. The Final Chamber (142207,
// lock 86) seals the path from the launch bay to Mekgineer Thermaplugg's
// chamber. Both are TYPE 0 (DOOR). UseGO opens them regardless of lock checks.
//
// Conditional events (no roster patch needed): the bot walks naturally down
// the dungeon corridor, the door appears in FindNearestGameObject range, and
// the condition fires — the bot walks up to it and clicks it open.

namespace
{
    constexpr uint32 GNOMER_WORKSHOP_DOOR      = 90858;
    constexpr uint32 GNOMER_FINAL_CHAMBER       = 142207;

    bool GnomerWorkshopDoor(Player* bot, AiObjectContext* /*context*/)
    {
        GameObject* door = bot->FindNearestGameObject(GNOMER_WORKSHOP_DOOR, 50.0f);
        if (!door)
            return false;
        if (door->GetGoState() != GO_STATE_READY)
            return false;
        if (!bot->IsWithinDistInMap(door, 45.0f))
            return false;
        return true;
    }

    bool GnomerFinalChamber(Player* bot, AiObjectContext* /*context*/)
    {
        GameObject* door = bot->FindNearestGameObject(GNOMER_FINAL_CHAMBER, 50.0f);
        if (!door)
            return false;
        if (door->GetGoState() != GO_STATE_READY)
            return false;
        if (!bot->IsWithinDistInMap(door, 45.0f))
            return false;
        return true;
    }
}

void RegisterGnomereganEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(90, 1, "Open Workshop Door")
                      .Conditional(&GnomerWorkshopDoor)
                      .UseGO(GNOMER_WORKSHOP_DOOR, 45.0f)
                      .Build());

    out.push_back(EventBuilder(90, 2, "Open The Final Chamber")
                      .Conditional(&GnomerFinalChamber)
                      .UseGO(GNOMER_FINAL_CHAMBER, 45.0f)
                      .Build());
}
