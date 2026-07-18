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
// encounter data bit (TYPE_FIRE1-4) and summons a wave of adds. When all 4
// are lit, the Aku'mai portal (GO 21117) opens and the party can proceed.
//
// PERSISTENT: each fire lighting summons adds — combat pauses the non-
// combat engine mid-event. Persistence keeps the step progress across the
// combat gap so the next fire fires after the adds are cleared, instead of
// rewinding to step 0.
//
// MoveTo before each UseGO: the DcObjectiveArriveAction calls StopBot(Hold)
// before driving the event, which cancels the embedded HopTo in a bare UseGO
// step. MoveTo drives the approach under the Hold so the tank reaches the
// fire, then UseGO clicks it.

void RegisterBlackfathomDeepsEvents(std::vector<DungeonEvent>& out)
{
    constexpr uint32 BFD_FIRE_1 = 21118;
    constexpr uint32 BFD_FIRE_2 = 21119;
    constexpr uint32 BFD_FIRE_3 = 21120;
    constexpr uint32 BFD_FIRE_4 = 21121;

    // Fire positions on the outer walkway:
    //   Fire 1: (-813.5, -158.5, -24.5)
    //   Fire 2: (-813.6, -170.5, -24.5)
    //   Fire 3: (-824.0, -170.4, -24.5)
    //   Fire 4: (-823.9, -158.5, -24.5)
    //
    // After all 4 fires are lit, 2 Aku'mai Servants (entry 4978) spawn from
    // the 4th fire. Kill both before teleporting the party to Aku'mai's
    // inner chamber. The fire room is separated from Aku'mai by water — the
    // navmesh can't path through it, so TeleportParty bypasses the gap.
    out.push_back(
        EventBuilder(48, 1, "Light the Fires of Aku'mai")
            .Anchored(/*encounterIndex*/ 1)  // after Gelihast (index 0)
            .Persistent()
            .MoveTo(-813.5f, -158.5f, -24.5f, /*radius*/ 5.0f)
            .UseGO(BFD_FIRE_1, /*searchRadius*/ 10.0f)
            .MoveTo(-813.6f, -170.5f, -24.5f, /*radius*/ 5.0f)
            .UseGO(BFD_FIRE_2, /*searchRadius*/ 10.0f)
            .MoveTo(-824.0f, -170.4f, -24.5f, /*radius*/ 5.0f)
            .UseGO(BFD_FIRE_3, /*searchRadius*/ 10.0f)
            .MoveTo(-823.9f, -158.5f, -24.5f, /*radius*/ 5.0f)
            .UseGO(BFD_FIRE_4, /*searchRadius*/ 10.0f)
            // Kill the 2 Aku'mai Servants that spawn from the final fire
            // before teleporting to the boss chamber.
            .KillCreatureEngage(/*Aku'mai Servant*/ 4978, /*count*/ 2, /*searchRadius*/ 80.0f)
            .TeleportParty(/*checkpoint@fires*/ -823.9f, -158.5f, -24.5f,
                           /*aku'mai's room*/ -848.0f, -454.0f, -34.0f)
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
    // (eventId 1) lights all 4 fires, teleporting the party to Aku'mai.
    //
    // Aku'mai is auto-derived from her static spawn at encounterIndex 1,
    // but the fires objective shares that same index. After the objective
    // is cleared the strict-greater advance skips past Aku'mai (also key 1).
    // Remove the auto-derived entry and re-add with orderOverride=2 so
    // she sorts after the fires at key 1.
    {
        BossRosterPatch p;
        p.mapId = 48;
        p.remove = { 4829 };
        p.add = {
            MakeObjective(OBJ(1), /*encounterIndex*/ 1, 48, "Fires of Aku'mai",
                          /*outer walkway between the 4 braziers*/ -818.7f, -164.5f, -24.5f,
                          /*arriveRadius*/ 30.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
            MakeBoss(4829, 48, "Aku'mai",
                     /*spawn position*/ -848.446f, -453.865f, -33.892f,
                     /*inheritCompletionFrom*/ 4829, /*orderOverride*/ 2),
        };
        t.push_back(std::move(p));
    }
}
