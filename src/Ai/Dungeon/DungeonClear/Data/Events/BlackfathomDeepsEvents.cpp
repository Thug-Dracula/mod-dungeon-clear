/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// --- Blackfathom Deeps (map 48) — the FOUR FIRES OF AKU'MAI --------------
// After Gelihast is dead, the party must light all 4 Fires of Aku'mai
// (GO 21118-21121) in the circular fire room around the Aku'mai portal.
// Each fire is a clickable GameObject; clicking it sets the corresponding
// encounter data bit (TYPE_FIRE1-4). When all 4 are lit, the Aku'mai portal
// (GO 21117) opens and the party can proceed to the final encounter.
//
// This event is ANCHORED to an objective in the fire room (after Gelihast's
// encounter index), so boss-nav drives the tank there. The steps walk to
// each fire and click it. Simple non-persistent: no combat interrupts the
// clicks since the trash is already clear.

#include "GameObject.h"
#include "InstanceScript.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

void RegisterBlackfathomDeepsEvents(std::vector<DungeonEvent>& out)
{
    constexpr uint32 BFD_FIRE_1 = 21118;
    constexpr uint32 BFD_FIRE_2 = 21119;
    constexpr uint32 BFD_FIRE_3 = 21120;
    constexpr uint32 BFD_FIRE_4 = 21121;

    // One event with four UseGO steps — one per fire. The bot walks to each
    // and clicks it. The search radius is generous so the GO finder locks
    // onto the correct fire from anywhere in the circular room.
    out.push_back(
        EventBuilder(48, 1, "Light the Fires of Aku'mai")
            .Anchored(/*encounterIndex*/ 1)  // after Gelihast (index 0)
            .UseGO(BFD_FIRE_1, /*searchRadius*/ 40.0f)
            .UseGO(BFD_FIRE_2, /*searchRadius*/ 40.0f)
            .UseGO(BFD_FIRE_3, /*searchRadius*/ 40.0f)
            .UseGO(BFD_FIRE_4, /*searchRadius*/ 40.0f)
            .Build());
}

// --- roster patch (relocated from BossRosterRegistry) --------------------
void RegisterBlackfathomDeepsRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // --- Blackfathom Deeps (map 48) — Fires of Aku'mai objective --------
    // Add the fire room centre as an OBJECTIVE anchor between Gelihast
    // (encounterIndex 0) and Aku'mai (encounterIndex 1, which shares the
    // same bit). Boss-nav drives the tank to the objective; the event
    // (eventId 1) lights all 4 fires, opening the portal.
    {
        BossRosterPatch p;
        p.mapId = 48;
        p.add = {
            MakeObjective(OBJ(1), /*encounterIndex*/ 1, 48, "Fires of Aku'mai",
                          /*fire room centre*/ -725.0f, 5.0f, -30.0f,
                          /*arriveRadius*/ 15.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
        };
        t.push_back(std::move(p));
    }
}
