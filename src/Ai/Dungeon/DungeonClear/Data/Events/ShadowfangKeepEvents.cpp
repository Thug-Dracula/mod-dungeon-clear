/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

#include <atomic>

// --- Shadowfang Keep (map 33) — CONDITIONAL, FACTION-SPECIFIC -------------
// The Courtyard Door (GO 18895) gates the keep past the entry rooms and is
// opened only by a freed prisoner, not by the party. The real mechanic
// (verified from the SFK SmartAI, 2026-06-11) is faction-specific — each side
// has its OWN lever + prisoner:
//   Alliance: pull lever 18901 (opens cell gate 18936) -> gossip Sorcerer
//             Ashcrombe (3850, menu 21213) option 0.
//   Horde:    pull lever 18900 (opens cell gate 18934) -> gossip Deathstalker
//             Adamant (3849, menu 21214) option 0.
// Picking the option fires the prisoner's GOSSIP_SELECT SmartAI: he walks to
// the courtyard door and, ~35s later (5s + 30s waypoint pauses), opens it. Two
// events, one per faction, each gated by a team-aware condition (1 = Alliance,
// 2 = Horde) so the right lever + prisoner drive; only one is ever due for a
// given party. The step list is lever -> gossip -> wait-for-door (an earlier
// version skipped the lever, so the bot could never reach the caged prisoner
// and the gossip was a no-op). Relevance 31 (DungeonClearEventDue) preempts the
// boss pull / door-blocked stall. Optional so a non-firing script degrades to
// the normal door-blocked stall.

namespace
{
    bool SfkCourtyardAlliance(Player* bot, AiObjectContext* context);
    bool SfkCourtyardHorde(Player* bot, AiObjectContext* context);
}

void RegisterShadowfangKeepEvents(std::vector<DungeonEvent>& out)
{
    // After freeing the prisoner, walk up to the courtyard door and wait THERE
    // (not in the cell) for it to open — the closed door stops the approach a
    // few yards short, so the tank is parked ready to walk through the instant
    // the prisoner opens it.
    // Panel placement: the courtyard door is opened right after the first boss
    // (Rethilgore, 3914) and gates the rest of the keep, so it sorts as its own
    // row immediately AFTER Rethilgore (#2) rather than last. Each row is shown
    // only to the faction that actually performs it — the activation predicate
    // already team-gates execution; .PanelTeam mirrors that on the panel so the
    // other faction's row is hidden.
    out.push_back(EventBuilder(33, 1, "Free Ashcrombe (Courtyard Door, Alliance)")
                      .Conditional(&SfkCourtyardAlliance)
                      .MoveTo(-248.0f, 2122.0f, 81.3f, /*radius*/ 6.0f)
                      .UseGO(/*lever*/ 18901, /*searchRadius*/ 14.0f)
                      .Gossip(/*Sorcerer Ashcrombe*/ 3850, /*option*/ 0, /*searchRadius*/ 16.0f)
                      .MoveTo(/*courtyard door*/ -242.58f, 2159.05f, 90.62f, /*radius*/ 9.0f)
                      .WaitForGOState(/*courtyard door*/ 18895, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 60000)
                      .PanelAfterBoss(/*Rethilgore*/ 3914)
                      .PanelTeam(TEAM_ALLIANCE)
                      .Optional()
                      .Build());

    out.push_back(EventBuilder(33, 2, "Free Adamant (Courtyard Door, Horde)")
                      .Conditional(&SfkCourtyardHorde)
                      .MoveTo(-251.0f, 2115.0f, 81.3f, /*radius*/ 6.0f)
                      .UseGO(/*lever*/ 18900, /*searchRadius*/ 14.0f)
                      .Gossip(/*Deathstalker Adamant*/ 3849, /*option*/ 0, /*searchRadius*/ 16.0f)
                      .MoveTo(/*courtyard door*/ -242.58f, 2159.05f, 90.62f, /*radius*/ 9.0f)
                      .WaitForGOState(/*courtyard door*/ 18895, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 60000)
                      .PanelAfterBoss(/*Rethilgore*/ 3914)
                      .PanelTeam(TEAM_HORDE)
                      .Optional()
                      .Build());
}

// --- Courtyard-door activation conditions (1 = Alliance, 2 = Horde) -------
// The Courtyard Door (GO 18895) blocks progression past the entry rooms and is
// opened not by the party but by a freed prisoner. The mechanic is
// FACTION-SPECIFIC: Alliance pulls the lever by Sorcerer Ashcrombe's cell
// (3850) and gossips him; Horde pulls a different lever by Deathstalker
// Adamant's cell (3849) and gossips him. Both NPCs spawn in AC, but each party
// uses its own — so the condition is gated on team so the right event (right
// lever + right prisoner) drives. DUE while the door is still shut
// (GO_STATE_READY) AND the faction's prisoner is alive to free; once the door
// opens this reads false and the event latches done.
namespace
{
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

        // Throttled diagnostic: one line / 5s per faction so a live run shows WHY
        // the event is or isn't due (door missing/open, no prisoner, first boss
        // still up). Lands in DungeonClear.log. atomic because bot AI ticks run on
        // the MapUpdate.Threads pool — the throttle stamp is read/written from
        // multiple map threads (the check-then-set race is benign: at worst two
        // lines in a window).
        static std::atomic<uint32> lastLog{0};
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
}
