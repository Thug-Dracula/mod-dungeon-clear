/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// --- Scarlet Monastery Cathedral (map 189) — ROOM-AGGRO PRE-CLEAR ---------
// Highlord Mograine's engage fires a grid AttackStart that drags the WHOLE
// cathedral into combat (CATHEDRAL_PULL_RANGE 80); pulling him before the room
// is clear wipes the party. Milestone 3 re-expresses that pre-clear (formerly
// the standalone RoomAggroRegistry path) as a CONDITIONAL room-aggro event:
// condition 3 (shared — see SharedConditions.cpp) reads DUE while the room-trash
// value (the RoomAggroRegistry geometry around the LIVE boss, minus the boss
// aggro sphere / unreachable / door-blocked, with the RoomClearTimeout give-up
// valve) still has anything to clear, and the lone KillCreature(0 = room-trash
// mode) step gates the boss pull until it is empty. DcRunEventAction drives the
// actual engage (nearest room trash first); the spatial logic stays in
// DungeonClearRoomTrashValue. Required (hold the pull until clear or the value
// gives up) and NEVER latched — the same row re-fires for each room-aggro boss
// on the map (Mograine, then Whitemane). This is the SM Cathedral test bed;
// other RoomAggroRegistry maps migrate by adding one more row each (and keep the
// legacy path until they do).

void RegisterScarletMonasteryEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(189, 1, "Clear the Cathedral (room-aggro pre-clear)")
                      .Conditional(&DcRoomAggroPreClearCondition)
                      .KillCreature(/*room trash*/ 0)
                      .Build());
}

// --- roster patch (relocated from BossRosterRegistry) --------------------
void RegisterScarletMonasteryRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

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
    //
    // ORDER FIX. Whitemane's DBC bit (5) sorts Mograine AFTER High
    // Inquisitor Fairbanks (4542, bit 4), so the auto path was
    // Fairbanks -> room-clear + Mograine. The run is much smoother the
    // other way: sweep the Reanimation chamber and kill Mograine FIRST,
    // then mop up Fairbanks last (he stands off in his own alcove and
    // pulls nothing of the main hall). Give Mograine orderOverride 3 so
    // he is picked before Fairbanks while still completing on Whitemane's
    // real kill-bit 5 (BossOrderKey uses the override; the completion mask
    // still keys on encounterIndex) — same decoupling as Stratholme's
    // Barthilas. Net Cathedral order: room-clear + Mograine -> Fairbanks.
    {
        BossRosterPatch p;
        p.mapId = 189;
        p.remove = { 3977 };
        p.add = {
            MakeBoss(3976, 189, "Scarlet Commander Mograine",
                     1153.9f, 1398.4f, 32.6f, /*completionFrom*/ 3977,
                     /*orderOverride*/ 3),
        };
        t.push_back(std::move(p));
    }
}
