/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonBossesValue.h"

#include <limits>
#include <unordered_set>

#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/Data/BossSpawnIndex.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Playerbots.h"

namespace
{
    // Snap each boss's spawn position onto the navmesh. DBC / creature_data
    // coordinates sometimes sit a few yards above the floor (alcove ledges,
    // platforms where the spawn was placed before mmap regen, etc.). Without
    // snapping, PathGenerator returns PATHFIND_FARFROMPOLY_END for those
    // bosses and Advance's at-boss trigger never matches the raw spawn pos.
    // 40yd is generous enough to catch flying-phase bosses whose anchor is
    // suspended in air (Eregos, Skadi's drake-phase landing) without
    // wandering into a different room.
    constexpr float BOSS_SNAP_RADIUS = 40.0f;

    std::vector<DungeonBossInfo> SnapAll(Map const* map, std::vector<DungeonBossInfo> bosses)
    {
        if (!map)
            return bosses;
        for (DungeonBossInfo& info : bosses)
        {
            NavmeshSnap::Result const r = NavmeshSnap::Snap(map, info.x, info.y, info.z, BOSS_SNAP_RADIUS);
            if (r.ok)
            {
                info.x = r.x;
                info.y = r.y;
                info.z = r.z;
            }
            // Snap failure leaves the original coords. The at-boss trigger
            // will then never fire for that boss and the stalled fallback
            // takes over — same behavior as before this change.
        }
        return bosses;
    }

    // For split maps (Dire Maul et al.) keep only the bosses of the wing the
    // bot is in. The wing is identified by proximity: the wings occupy
    // far-apart regions of the map and the bot enters directly into one, so the
    // nearest registered boss is reliably in-wing — including its dead/cleared
    // bosses, whose spawn coords are static, so the choice stays stable as the
    // bot clears its way through. Maps with no wing registration pass through.
    std::vector<DungeonBossInfo> FilterToCurrentWing(Player* bot, uint32 mapId,
                                                     std::vector<DungeonBossInfo> bosses)
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(mapId);
        if (!layout || layout->wings.empty() || bosses.empty())
            return bosses;

        // Connected-wing maps (Maraudon) are not split for filtering: every
        // wing is reachable from any entrance, so all bosses must stay in the
        // list. The wing data is a display label only — never filter, or the
        // bot would clear one region and read the whole dungeon as done.
        if (!layout->isolated)
            return bosses;

        std::vector<DungeonWing> const& wings = layout->wings;

        // Pick the wing owning the boss nearest the bot.
        size_t bestWing = wings.size();
        float bestDistSq = std::numeric_limits<float>::max();
        for (size_t w = 0; w < wings.size(); ++w)
        {
            for (uint32 entry : wings[w].bossEntries)
            {
                for (DungeonBossInfo const& b : bosses)
                {
                    if (b.entry != entry)
                        continue;
                    float const dx = b.x - bot->GetPositionX();
                    float const dy = b.y - bot->GetPositionY();
                    float const dz = b.z - bot->GetPositionZ();
                    float const d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bestDistSq)
                    {
                        bestDistSq = d2;
                        bestWing = w;
                    }
                }
            }
        }

        // No registered boss matched the live list — leave it untouched rather
        // than blanking it (would falsely read as "all cleared").
        if (bestWing >= wings.size())
            return bosses;

        std::unordered_set<uint32> const keep(wings[bestWing].bossEntries.begin(),
                                              wings[bestWing].bossEntries.end());
        std::vector<DungeonBossInfo> filtered;
        filtered.reserve(keep.size());
        for (DungeonBossInfo const& b : bosses)
            if (keep.count(b.entry))
                filtered.push_back(b);

        LOG_DEBUG("playerbots.dungeonclear",
                  "[dungeon-clear] map {} split into wings; bot in '{}' — "
                  "{} of {} bosses kept", mapId, wings[bestWing].name,
                  filtered.size(), bosses.size());

        // Defensive: if the chosen wing somehow contributed nothing, keep the
        // full list so the bot still has a target.
        return filtered.empty() ? bosses : filtered;
    }
}

std::vector<DungeonBossInfo> DungeonBossesValue::Calculate()
{
    if (!bot || !bot->IsInWorld())
        return {};

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return {};

    uint32 const mapId = bot->GetMapId();
    Difficulty const difficulty = map->GetDifficulty();

    // Apply per-dungeon roster corrections (remove wrong/event-locked bosses,
    // add real bosses or travel objectives) before wing-filtering and snapping.
    std::vector<DungeonBossInfo> roster =
        BossRosterRegistry::Apply(mapId, BossSpawnIndex::Get(mapId, difficulty));

    return SnapAll(map, FilterToCurrentWing(bot, mapId, std::move(roster)));
}
