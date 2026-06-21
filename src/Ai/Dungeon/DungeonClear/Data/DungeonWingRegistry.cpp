/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonWingRegistry.h"

#include <unordered_map>

#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

namespace
{
    // mapId -> wings. Boss entries are the ENCOUNTER_CREDIT_KILL_CREATURE
    // credit-entries from instance_encounters, i.e. exactly what BossSpawnIndex
    // keys its list on, so a wing's entries match the "dungeon bosses" output
    // 1:1. Every boss of a split map must appear in exactly one wing — any
    // entry left out would silently never be cleared.
    std::unordered_map<uint32, DungeonWingLayout> const& Store()
    {
        static std::unordered_map<uint32, DungeonWingLayout> const store = {
            // --- Dire Maul (map 429) -------------------------------------
            // Three wings, each entered through its own portal; no in-instance
            // route connects them. Grouping verified against creature spawn
            // coords (East sits at y < -100, West at x < 150 / y > 400, North
            // at x > 300), which is also why nearest-boss wing detection is
            // unambiguous. isolated == true: filter to the bot's wing.
            {429, {true, {
                {"Dire Maul (East)", {
                    11490,  // Zevrim Thornhoof
                    13280,  // Hydrospawn
                    14327,  // Lethtendris
                    11492,  // Alzzin the Wildshaper
                    // Ironbark / Conservatory Door travel objective (a synthetic
                    // entry, not a creature) — keep it in-wing so wing-filtering
                    // doesn't drop it. See BossRosterRegistry map-429 patch.
                    BossRosterRegistry::ObjectiveEntry(1),
                }},
                {"Dire Maul (West)", {
                    11489,  // Tendris Warpwood
                    11488,  // Illyanna Ravenoak
                    11487,  // Magister Kalendris
                    11496,  // Immol'thar
                    11486,  // Prince Tortheldrin
                }},
                {"Dire Maul (North)", {
                    14326,  // Guard Mol'dar
                    14322,  // Stomper Kreeg
                    14321,  // Guard Fengus
                    14323,  // Guard Slip'kik
                    14325,  // Captain Kromcrush
                    14324,  // Cho'Rush the Observer
                    11501,  // King Gordok
                    // Gordok Courtyard / Inner Door travel objectives (synthetic
                    // entries, not creatures) — keep them in-wing so wing-filtering
                    // doesn't drop them. See BossRosterRegistry map-429 patch.
                    BossRosterRegistry::ObjectiveEntry(2),
                    BossRosterRegistry::ObjectiveEntry(3),
                }},
            }}},

            // --- Scarlet Monastery (map 189) -----------------------------
            // Four wings, each entered through its own portal off the shared
            // outdoor courtyard; you must leave to the courtyard to switch, so
            // no in-instance route connects them. The wing clusters sit far
            // apart in world space — Graveyard (x~1800, y~1270) and Cathedral
            // (x~1160, y~1370) in the north half, Library (x~130, y~-345) and
            // Armory (x~1965, y~-430) in the south half, each 600+ yds from the
            // others — so nearest-boss wing detection is unambiguous.
            // isolated == true: filter to the bot's wing.
            //
            // Entries are the kill-creature credit-entries from
            // instance_encounters (what BossSpawnIndex emits) PLUS any boss
            // injected by BossRosterRegistry. The Cathedral's tracked encounters
            // are Fairbanks and Whitemane; the roster patch removes Whitemane
            // (event-locked) and injects Scarlet Commander Mograine (3976), so
            // 3976 is listed here too — otherwise the wing filter, which runs
            // after the patch, would drop the injected boss.
            {189, {true, {
                {"Scarlet Monastery (Graveyard)", {
                    3983,   // Interrogator Vishas
                    4543,   // Bloodmage Thalnos
                }},
                {"Scarlet Monastery (Library)", {
                    3974,   // Houndmaster Loksey
                    6487,   // Arcanist Doan
                }},
                {"Scarlet Monastery (Armory)", {
                    3975,   // Herod
                }},
                {"Scarlet Monastery (Cathedral)", {
                    4542,   // High Inquisitor Fairbanks
                    3976,   // Scarlet Commander Mograine (injected by roster patch)
                    3977,   // High Inquisitor Whitemane (removed by patch; kept for wing detection)
                }},
            }}},

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
            {349, {false, {
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
            }}},
        };
        return store;
    }
}

DungeonWingLayout const* DungeonWingRegistry::Get(uint32 mapId)
{
    auto const& s = Store();
    auto it = s.find(mapId);
    return it == s.end() ? nullptr : &it->second;
}

std::string DungeonWingRegistry::WingName(uint32 mapId, uint32 bossEntry)
{
    DungeonWingLayout const* layout = Get(mapId);
    if (!layout)
        return "";
    for (DungeonWing const& wing : layout->wings)
        for (uint32 entry : wing.bossEntries)
            if (entry == bossEntry)
                return wing.name;
    return "";
}
