/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"

#include "GameObject.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"

#include <unordered_map>

// --- Maraudon (map 349) — Portal to Inner Maraudon (Pristine Waters) ------
// After the party clears the Purple side (Celebras the Cursed), the Portal to
// Inner Maraudon (178404, type 22 spell caster, spell 21128) teleports the
// party from the waterfall room into Pristine Waters. Celebras's encounter bit
// gate ensures this fires only after he is dead. TeleportParty avoids stranding
// followers on the far side of the portal.

namespace
{
    constexpr uint32 MARAUDON_PORTAL          = 178404;
    // Celebras the Cursed — DungeonEncounter bit. Teleport fires only after
    // he is dead and the seal is open.
    constexpr uint32 MARAUDON_CELEBRAS_ENTRY  = 12225;
    constexpr uint32 MARAUDON_CELEBRAS_BIT    = 0;  // correct bit unknown; gate entry is safer

    bool MaraudonPortal(Player* bot, AiObjectContext* /*context*/)
    {
        // Already on the far side (Pristine Waters / Earth Song Falls area).
        // The portal landing is at the Pristine Waters entrance ~(386, 33, -131).
        // If the bot is within ~100yd of those coords, skip.
        if (bot->GetExactDist(386.27f, 33.4144f, -130.934f) < 100.0f)
            return false;

        // Only fire if Celebras is dead (his corpse is gone but the seal is open).
        Creature* celebras = bot->FindNearestCreature(MARAUDON_CELEBRAS_ENTRY, 500.0f, false);
        if (celebras)
            return false;  // still alive

        // The portal must exist and be usable.
        GameObject* portal = bot->FindNearestGameObject(MARAUDON_PORTAL, 60.0f);
        if (!portal)
            return false;

        // The tank must be close enough to trigger the portal use.
        if (!bot->IsWithinDistInMap(portal, 50.0f))
            return false;

        return true;
    }
}

void RegisterMaraudonEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(349, 1, "Portal to Pristine Waters")
                      .Conditional(&MaraudonPortal)
                      .UseGO(MARAUDON_PORTAL, 50.0f)
                      .Build());
}

// --- wing layout (relocated from DungeonWingRegistry) --------------------
void RegisterMaraudonWings(std::unordered_map<uint32, DungeonWingLayout>& store)
{
    // --- Maraudon (map 349) --------------------------------------
    // Three named regions — Orange (Foulspore Cavern), Purple (Wicked
    // Grotto) and the inner Pristine Waters (Earth Song Falls) — but
    // UNLIKE Dire Maul they share one connected interior: orange and
    // purple converge at the Celebras seal, which opens into the inner
    // waters, and the Earth Song Falls surface portal drops straight
    // into that same inner area. So every boss is reachable from any
    // entrance.
    //
    // isolated == false: this is a LABEL only. Do NOT filter — all
    // eight bosses stay in the clear list; the wing name is surfaced in
    // status/UI so the player can see which region each boss sits in.
    //
    // Entries are the kill-creature credit-entries from
    // instance_encounters (what BossSpawnIndex emits). Celebras the
    // Cursed sits at the purple-side seal he unlocks, so he is grouped
    // with Purple.
    store[349] = {false, {
        {"Maraudon (Orange)", {
            13282,  // Noxxion
            12258,  // Razorlash
        }},
        {"Maraudon (Purple)", {
            12236,  // Lord Vyletongue
            12225,  // Celebras the Cursed
        }},
        {"Maraudon (Pristine Waters)", {
            13601,  // Tinkerer Gizlock
            12203,  // Landslide
            13596,  // Rotgrip
            12201,  // Princess Theradras
        }},
    }};
}
