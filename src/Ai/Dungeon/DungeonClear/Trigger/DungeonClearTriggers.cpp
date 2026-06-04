/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearTriggers.h"

#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Playerbots.h"

namespace
{
    // Asymmetric ranges so a trash pack sitting just outside the boss room
    // gets engaged before the at-boss trigger fires. 22yd is just outside
    // most level-80 elite aggro radii (~18-20yd), giving room to position
    // before melee; 35yd for trash catches packs one tick-cycle earlier.
    constexpr float DC_ENGAGE_RANGE = 22.0f;
    constexpr float DC_TRASH_CONE_RANGE = 35.0f;
    constexpr float DC_TRASH_CONE_HALF_ANGLE = static_cast<float>(M_PI) / 3.0f;  // 60°

    // When true, evaluate "blocking trash" via the bot's actual mmap path
    // polyline instead of the geometric cone. Catches packs around corners
    // and avoids "pack on the other side of a wall" false positives.
    // Falls back to the cone scan when path computation fails.
    constexpr bool  DC_USE_CORRIDOR_SCAN = true;
    constexpr float DC_CORRIDOR_LOOKAHEAD = 35.0f;
    // Widened from 8 to 18 to roughly match elite aggro radius — see the
    // matching constant in DungeonClearActions.cpp. The trigger and action use
    // the same FindBlockingTrashOnPath band, so these must stay in sync.
    constexpr float DC_CORRIDOR_WIDTH = 18.0f;

    // Between-pulls wait policy. Tank holds advance/trash-engage until the party
    // has caught up, rested, and tank-side loot is collected. The HP/mana
    // recovery thresholds come from DungeonClearUtil::RestMin{Hp,Mp}Pct(), which
    // clamp to mod-playerbots' drink/eat targets (see DungeonClearUtil.h).
    // Max distance the tank may lead a party member before it holds the advance
    // to let them catch up. Configurable; see DungeonClear.PartyMaxSpread. Must
    // stay aligned with the same key's default in DungeonClearActions.cpp.
    constexpr float DC_PARTY_MAX_SPREAD_DEFAULT = 25.0f;

    // DC must be enabled AND not paused for the driving ladder to fire. Pause
    // is a soft stop: `enabled` (and all boss progress) stays set, but every
    // trigger here goes inert so the tank holds exactly as it would under
    // `dc off`. See DungeonClearPausedValue.
    bool IsEnabled(AiObjectContext* context)
    {
        return AI_VALUE(bool, "dungeon clear enabled") && !AI_VALUE(bool, "dungeon clear paused");
    }

    // Distance from the bot to the boss's LIVE creature position when it is
    // loaded on the map, else to its static DB spawn coords. The advance action
    // pursues this same effective position, so the trigger ladder and the action
    // must agree on "are we at the boss yet" — otherwise a wandering/patrolling
    // boss that has left its spawn anchor makes the action keep walking while the
    // at-boss trigger thinks it has arrived (or vice versa).
    float BossEngageDistance(Player* bot, AiObjectContext* context, DungeonBossInfo const& boss)
    {
        if (Creature* live = DungeonClearUtil::GetLiveBoss(bot, context, boss.entry))
            return bot->GetDistance(live->GetPositionX(), live->GetPositionY(), live->GetPositionZ());
        return bot->GetDistance(boss.x, boss.y, boss.z);
    }

    bool IsBetweenPullsReady(Player* bot, AiObjectContext* context)
    {
        if (AI_VALUE(bool, "has available loot"))
            return false;
        float const maxSpread = sConfigMgr->GetOption<float>(
            "DungeonClear.PartyMaxSpread", DC_PARTY_MAX_SPREAD_DEFAULT);
        return DungeonClearUtil::IsPartyReady(bot, DungeonClearUtil::RestMinHpPct(),
                                              DungeonClearUtil::RestMinMpPct(), maxSpread);
    }
}

bool DungeonClearIdleTrigger::IsActive()
{
    if (!IsEnabled(context))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    // Fires whenever DC is on and combat is over. The advance action itself
    // decides between walking, holding (rest/loot/catch-up), or yielding to
    // the higher-priority at-boss trigger when in engage range and ready.
    // Always claiming this engine slot keeps grind/new-rpg from stealing the
    // tank during the wait.
    return true;
}

bool DungeonClearAtBossTrigger::IsActive()
{
    if (!IsEnabled(context))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    float const engageRange =
        DungeonClearUtil::BossEngageRange(bot, context, *next, DC_ENGAGE_RANGE);
    if (BossEngageDistance(bot, context, *next) >= engageRange)
        return false;

    // When the long-path cache is anchored (registered route), make sure
    // all intermediate anchors have been resolved before firing. This
    // prevents the bot from "engaging" a boss it's geometrically near but
    // separated from by a wall or door — the cached anchor list runs the
    // bot around through the actual corridor first.
    ChunkedPathfinder::Result const& path =
        AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
    if (path.reachable && !path.segments.empty())
    {
        bool anyAnchored = false;
        for (PathSegment const& seg : path.segments)
            if (seg.anchored) { anyAnchored = true; break; }
        if (anyAnchored)
        {
            // Last segment is the boss anchor; everything before it must
            // be within its arriveRadius.
            for (size_t i = 0; i + 1 < path.segments.size(); ++i)
            {
                PathSegment const& seg = path.segments[i];
                if (!seg.anchored)
                    continue;
                float const d = bot->GetDistance(seg.ex, seg.ey, seg.ez);
                if (d > seg.arriveRadius)
                    return false;
            }
        }
    }

    // Don't pull while party is still recovering or tank-side loot is pending.
    // The idle-trigger's advance action holds the tank in place meanwhile.
    return IsBetweenPullsReady(bot, context);
}

bool DungeonClearBlockingTrashTrigger::IsActive()
{
    if (!IsEnabled(context))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    // Within engage range of the live boss, at-boss trigger handles the pull.
    float const engageRange =
        DungeonClearUtil::BossEngageRange(bot, context, *next, DC_ENGAGE_RANGE);
    if (BossEngageDistance(bot, context, *next) < engageRange)
        return false;

    // Wait between pulls for loot, party catch-up, and rest.
    if (!IsBetweenPullsReady(bot, context))
        return false;

    // Prefer the wider DC-gated scan — packs at the far end of long
    // dungeon corridors fall outside the default 100yd sightDistance cap
    // that drives `possible targets`. Falls back to `possible targets`
    // when far-targets is empty (first tick, before its 500ms poll has
    // run).
    GuidVector const& farTargets = AI_VALUE(GuidVector, "dungeon clear far targets");
    GuidVector const& possibleTargets = AI_VALUE(GuidVector, "possible targets");
    GuidVector const& candidates = farTargets.empty() ? possibleTargets : farTargets;

    if (DC_USE_CORRIDOR_SCAN)
    {
        // Walk the cached long-path polyline. The polyline spans the
        // entire chunked route — anchored or anchor-free — so blocking
        // trash beyond a single PathGenerator call is still detected.
        ChunkedPathfinder::Result const& path =
            AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
        if (path.reachable && !path.segments.empty())
        {
            return DungeonClearUtil::FindBlockingTrashOnPath(
                bot, path.segments, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates) != nullptr;
        }
        // No usable long-path cache — fall back to a single-shot corridor
        // computed inline so the trigger stays live in degraded conditions.
        Movement::PointsArray corridor;
        if (DungeonClearUtil::ComputeCorridor(bot, next->x, next->y, next->z, corridor))
        {
            return DungeonClearUtil::FindBlockingTrashCorridor(
                bot, corridor, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates) != nullptr;
        }
        // Fall through to the cone scan below.
    }

    return DungeonClearUtil::FindBlockingTrash(bot, *next, DC_TRASH_CONE_RANGE,
                                               DC_TRASH_CONE_HALF_ANGLE,
                                               candidates) != nullptr;
}

bool DungeonClearPartyDiedTrigger::IsActive()
{
    if (!IsEnabled(context))
        return false;
    if (!bot)
        return false;

    if (bot->isDead())
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;
        if (member->GetMapId() != bot->GetMapId())
            continue;
        if (member->isDead())
            return true;
    }
    return false;
}

bool DungeonClearAllClearedTrigger::IsActive()
{
    if (!IsEnabled(context))
        return false;
    if (!bot)
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    auto const& bosses = AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");
    if (bosses.empty())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    return !next.has_value();
}

bool DungeonClearStalledTrigger::IsActive()
{
    if (!IsEnabled(context))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Only fall back when there is an actual stall reason set by Advance or
    // EngageBoss. If the path is clear, this trigger never fires.
    std::string const& reason = AI_VALUE(std::string&, "dungeon clear stall reason");
    if (reason.empty())
        return false;

    // Wait between pulls for loot, party catch-up, and rest before pulling
    // anything new — same gating the trash/boss triggers use.
    return IsBetweenPullsReady(bot, context);
}

bool DungeonClearDoorBlockedTrigger::IsActive()
{
    if (!IsEnabled(context))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Non-empty GUID means the blocking-door value found a closed door
    // within the corridor. Empty means clear path. Polled at 500ms by the
    // value itself, so this trigger is cheap.
    ObjectGuid const door = AI_VALUE(ObjectGuid, "dungeon clear blocking door");
    return !door.IsEmpty();
}

bool DungeonClearFollowTankTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;

    // Only redirect non-tank bots — the tank obviously doesn't follow itself.
    if (PlayerbotAI::IsTank(bot))
        return false;

    Player* tank = AI_VALUE(Player*, "dungeon clear party tank");
    if (tank && tank != bot)
    {
        // No distance gate: while DC is active on the tank, the follow-tank
        // action runs every non-combat tick. MovementAction::Follow yields a
        // no-op when already in range, so this is cheap. Without it, the
        // follower's `move from group` (anti-collision) preempts the default
        // `follow` strategy at melee range and they drift backward each tick.
        return true;
    }

    // No DC tank anymore. Keep firing for the one teardown tick needed to
    // cancel the leftover continuous MoveFollow this bot installed while it
    // was following the tank — MoveFollow is a persistent MotionMaster order,
    // and a self-bot (master == itself) never replaces it via its ordinary
    // follow, so without this it stays glued to the tank after dc off. The
    // action clears that GUID once it tears the follow down, so this stops
    // firing immediately after.
    return !AI_VALUE(ObjectGuid, "dungeon clear followed tank").IsEmpty();
}
