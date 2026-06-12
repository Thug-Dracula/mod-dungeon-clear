/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "EventConditionRegistry.h"

#include <unordered_map>

#include "Creature.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

namespace
{
    // --- Shadowfang Keep: courtyard door (conditions 1 = Alliance, 2 = Horde) -
    // The Courtyard Door (GO 18895) blocks progression past the entry rooms and
    // is opened not by the party but by a freed prisoner. The mechanic is
    // FACTION-SPECIFIC: Alliance pulls the lever by Sorcerer Ashcrombe's cell
    // (3850) and gossips him; Horde pulls a different lever by Deathstalker
    // Adamant's cell (3849) and gossips him. Both NPCs spawn in AC, but each
    // party uses its own — so the condition is gated on team so the right event
    // (right lever + right prisoner) drives. DUE while the door is still shut
    // (GO_STATE_READY) AND the faction's prisoner is alive to free; once the
    // door opens this reads false and the event latches done.
    constexpr uint32 SFK_COURTYARD_DOOR = 18895;
    constexpr uint32 SFK_PRISONER_ASHCROMBE = 3850;  // Alliance
    constexpr uint32 SFK_PRISONER_ADAMANT = 3849;    // Horde
    // Rethilgore (3914) is the first boss and stands AMONG the prison cells, so
    // the courtyard event must wait until he is dead — otherwise the party would
    // detour to the prison the instant DC is enabled at the zone entrance (the
    // bug this gate fixes). His grid is co-loaded with the prisoners', so the
    // prisoner-alive check below guarantees we are not reading a false "dead"
    // from an unloaded grid.
    constexpr uint32 SFK_FIRST_BOSS_RETHILGORE = 3914;
    // Door / prisoner sit near the entry; scan generously so the condition is
    // true from the moment the party is anywhere in the early keep.
    constexpr float SFK_SCAN = 200.0f;

    bool SfkCourtyard(Player* bot, TeamId team, uint32 prisonerEntry, char const* who)
    {
        if (bot->GetTeamId() != team)
            return false;

        GameObject* door = bot->FindNearestGameObject(SFK_COURTYARD_DOOR, SFK_SCAN);
        Creature* prisoner = bot->FindNearestCreature(prisonerEntry, SFK_SCAN, /*alive*/ true);
        bool const firstBossDead =
            DcTargeting::FindLiveCreatureOnMap(bot, SFK_FIRST_BOSS_RETHILGORE) == nullptr;

        // Throttled diagnostic (single-threaded world tick): one line / 5s per
        // faction so a live run shows WHY the event is or isn't due (door
        // missing/open, no prisoner, first boss still up). Lands in DungeonClear.log.
        static uint32 lastLog = 0;
        uint32 const now = getMSTime();
        if (getMSTimeDiff(lastLog, now) >= 5000)
        {
            lastLog = now;
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] SFK courtyard cond ({}): door={} state={} prisoner={} rethilgore={}",
                      bot->GetName(), who, door ? "found" : "MISSING",
                      door ? static_cast<int>(door->GetGoState()) : -1,
                      prisoner ? "alive" : "no", firstBossDead ? "dead" : "ALIVE");
        }

        if (!firstBossDead)
            return false;  // wait until the first boss (Rethilgore) is down
        if (!door || door->GetGoState() != GO_STATE_READY)
            return false;  // no door found, or already open
        return prisoner != nullptr;
    }

    bool SfkCourtyardAlliance(Player* bot, AiObjectContext* /*context*/)
    {
        return SfkCourtyard(bot, TEAM_ALLIANCE, SFK_PRISONER_ASHCROMBE, "A");
    }

    bool SfkCourtyardHorde(Player* bot, AiObjectContext* /*context*/)
    {
        return SfkCourtyard(bot, TEAM_HORDE, SFK_PRISONER_ADAMANT, "H");
    }

    // conditionId -> predicate. Add a row and reference its id from a Conditional
    // event's .Conditional(id) in DungeonEventRegistry.
    std::unordered_map<uint32, EventConditionRegistry::Condition> const& Conditions()
    {
        static std::unordered_map<uint32, EventConditionRegistry::Condition> const kConditions = {
            { 1, &SfkCourtyardAlliance },
            { 2, &SfkCourtyardHorde },
        };
        return kConditions;
    }
}

bool EventConditionRegistry::Evaluate(uint32 id, Player* bot, AiObjectContext* context)
{
    if (id == 0 || !bot)
        return false;

    auto const& conditions = Conditions();
    auto it = conditions.find(id);
    if (it == conditions.end() || !it->second)
        return false;

    return it->second(bot, context);
}

bool EventConditionRegistry::Has(uint32 id)
{
    return id != 0 && Conditions().count(id) != 0;
}
