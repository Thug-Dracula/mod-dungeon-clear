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
#include "ObjectMgr.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Playerbots.h"

namespace
{
    // Shared tuning constants (DC_ENGAGE_RANGE, DC_TRASH_CONE_*,
    // DC_USE_CORRIDOR_SCAN, DC_CORRIDOR_*, DC_PULL_START_RANGE)
    // now live in DungeonClearTuning.h so the
    // trigger ladder and the action layer cannot drift. See that header for the
    // unit/why annotations.

    // DC must be enabled AND not paused for the driving ladder to fire. Pause
    // is a soft stop: `enabled` (and all boss progress) stays set, but every
    // trigger here goes inert so the tank holds exactly as it would under
    // `dc off`. See DungeonClearPausedValue.
    //
    // The driving ladder also runs ONLY on the elected leader. In a raid several
    // tanks can have the flag set, but exactly one drives — the others follow it
    // like any other member (see DungeonClearFollowTankTrigger). Only the leader
    // ever reaches the leadership scan: followers don't set `enabled`, so they
    // short-circuit on the first check. See DcLeaderSignal::FindLeaderTank.
    bool IsEnabled(AiObjectContext* context, Player* bot)
    {
        if (!AI_VALUE(bool, "dungeon clear enabled") || AI_VALUE(bool, "dungeon clear paused"))
            return false;
        return DcLeaderSignal::IsDungeonClearLeader(bot);
    }

    // Trigger-side between-pulls gate. Thin wrapper over the shared
    // DcPartyState::IsBetweenPullsReady (one body for the trigger ladder and the
    // advance action, which had drifted as two copies). The trigger side also
    // requires no pending loot — never start a pull over a corpse — while the
    // action side deliberately does not (its Execute owns loot behind a
    // commit-timeout).
    bool IsBetweenPullsReady(Player* bot, AiObjectContext* context)
    {
        // Memoised within the tick: this strict gate is read by five triggers in
        // one tick and each does a full party walk. See DcTickMemo.
        return DcTickMemoAccess::BetweenPullsReady(bot, context, /*requireNoLoot*/ true);
    }
}

bool DungeonClearIdleTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
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
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    // Travel objectives are not combat targets — the at-objective trigger owns
    // arrival. Stand down so engage-boss never fires on a non-creature anchor.
    if (next->kind != DungeonAnchorKind::Boss)
        return false;

    // Close enough AND on the boss's own floor (not just 3D-near while passing
    // under an upper-floor boss). See IsAtBossEngage.
    if (!DcTickMemoAccess::AtBossEngage(bot, context, *next))
        return false;

    // Room-wide-aggro boss (RoomAggroRegistry): on engage it force-pulls the
    // whole room, so HOLD the boss pull while any room trash remains. The
    // room-clear driver (Off/Leeroy) or the pull pipeline (advanced/dynamic)
    // clears it first; the gate reopens the instant the room is clear (or the
    // RoomClearTimeout valve fires inside the value). Cheap cached read.
    if (RoomAggroRegistry::Find(bot->GetMapId(), next->entry) &&
        !AI_VALUE(GuidVector, "dungeon clear room trash remaining").empty())
        return false;

    // A closed door between us and the boss means it's BEYOND it and not actually
    // reachable yet — even when it's within straight-line engage range (a boss or
    // its pack right behind the door). Without this, IsAtBossEngage (a pure
    // distance+floor test, door-blind) fires the at-boss engage, which outranks
    // door-blocked (30 vs 22) and bee-lines the tank onto/through the door. Stand
    // down so door-blocked parks at its stand-off; re-evaluates the instant the
    // door opens. Checked fresh, not via the 500ms-cached blocking-door value,
    // which can still read empty the moment the boss first comes into range.
    Creature* const liveBoss = DcTargeting::GetLiveBoss(bot, context, next->entry);
    float const bx = liveBoss ? liveBoss->GetPositionX() : next->x;
    float const by = liveBoss ? liveBoss->GetPositionY() : next->y;
    float const bz = liveBoss ? liveBoss->GetPositionZ() : next->z;
    if (DcEngageGeometry::ClosedDoorBetween(bot, bx, by, bz))
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

bool DungeonClearAtObjectiveTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value() || next->kind != DungeonAnchorKind::Objective)
        return false;

    // Satisfied when the tank has reached the anchor (within arriveRadius), or
    // when the optional gate creature has spawned alive (the event already fired
    // its result, e.g. the real boss is up), so we don't need to babysit it.
    float const radius = next->arriveRadius > 0.0f
                             ? next->arriveRadius
                             : DcSettings::GetFloat(bot, "ObjectiveArriveRadius");
    if (bot->GetExactDist(next->x, next->y, next->z) <= radius)
        return true;

    if (next->gateEntry)
    {
        for (auto const& kv : map->GetCreatureBySpawnIdStore())
        {
            Creature* c = kv.second;
            if (c && c->GetEntry() == next->gateEntry && c->IsAlive())
                return true;
        }
    }
    return false;
}

bool DungeonClearEventDueTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    // Leader drives events; followers stay on follow-tank.
    if (!DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;
    if (!DungeonEventRegistry::HasEvents(map->GetId()))
        return false;

    // Hand the trigger and the action the SAME "which event is due" answer so
    // they can never disagree about whether to fire / what to drive.
    return DungeonEventExecutor::FindDueConditionalEvent(bot, context, map->GetId()) != nullptr;
}

bool DungeonClearBlockingTrashTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    // At the boss (close AND on its floor), the at-boss trigger handles the
    // pull — don't also scan for blocking trash. While merely passing under an
    // upper-floor boss this is false, so trash on the way to the ramp still gets
    // cleared. See IsAtBossEngage.
    if (DcTickMemoAccess::AtBossEngage(bot, context, *next))
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

    Unit* trash = nullptr;
    if (DC_USE_CORRIDOR_SCAN)
    {
        // Walk the cached long-path polyline. The polyline spans the
        // entire chunked route — anchored or anchor-free — so blocking
        // trash beyond a single PathGenerator call is still detected.
        ChunkedPathfinder::Result const& path =
            AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
        if (path.reachable && !path.segments.empty())
        {
            trash = DcTargeting::FindBlockingTrashOnPath(
                bot, path.segments, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
        }
        // No usable long-path cache — fall back to a single-shot corridor
        // computed inline so the trigger stays live in degraded conditions.
        else
        {
            Movement::PointsArray corridor;
            if (DcEngageGeometry::ComputeCorridor(bot, next->x, next->y, next->z, corridor))
                trash = DcTargeting::FindBlockingTrashCorridor(
                    bot, corridor, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
            else
                trash = DcTargeting::FindBlockingTrash(
                    bot, *next, DC_TRASH_CONE_RANGE, DC_TRASH_CONE_HALF_ANGLE, candidates);
        }
    }
    else
        trash = DcTargeting::FindBlockingTrash(
            bot, *next, DC_TRASH_CONE_RANGE, DC_TRASH_CONE_HALF_ANGLE, candidates);

    if (!trash)
        return false;

    // Don't engage a pack on the FAR side of a closed door. Some doors aren't
    // modeled as solid in the navmesh (the tank can clip through), so the scan
    // finds the far-side pack and engage-trash (priority 25) would otherwise
    // out-prioritise door-blocked (22) and run the tank through the door to it,
    // dragging the group into the fight. Checked FRESH (not via the 500ms-cached
    // blocking-door value), because the door often isn't flagged yet at the very
    // tick the scan first sees the pack — which let the tank run through, clear
    // it, and walk back. With the pack vetoed, door-blocked parks at the door;
    // this re-evaluates the instant the door opens.
    if (DcEngageGeometry::ClosedDoorBetween(bot, trash->GetPositionX(),
                                            trash->GetPositionY(), trash->GetPositionZ()))
    {
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] blocking-trash: vetoed pack {} ({:.1f}yd) — closed door "
                  "between us and it on our floor",
                  bot->GetName(), trash->GetGUID().ToString(), bot->GetExactDist(trash));
        return false;
    }

    // Patrol-wait hold (dynamic pull decision == 3): the pull pipeline is holding
    // the tank at commit range to let a patrol pass before committing the pull.
    // Stand down unconditionally so the tank doesn't walk in and engage mid-wait.
    if (AI_VALUE(DcPullContext&, "dungeon clear pull context").decision == 3u)
    {
        DC_PULL_DEBUG("[DC:{}] blocking-trash: patrol-wait hold -> stand down",
                      bot->GetName());
        return false;
    }

    // In advanced-pull mode the pull pipeline OWNS trash packs: it LOS-pulls them
    // to camp rather than engaging in place. Stand down so the tank glides in
    // under Advance until the pull-start range, instead of walking up and fighting
    // here (engage-trash outranks advance, so without this it would preempt the
    // pull for any pack in the 20-35yd band the pull is deliberately waiting to
    // close). The one exception is a pack a prior pull gave up on (abort target):
    // fall through to the normal walk-in so the run never livelocks on it.
    if (AI_VALUE(bool, "dungeon clear pull mode current") &&
        trash->GetGUID() != AI_VALUE(DcPullContext&, "dungeon clear pull context").abortTarget)
    {
        DC_PULL_DEBUG("[DC:{}] blocking-trash: pull mode owns pack {} ({:.1f}yd) -> "
                      "stand down for the pull pipeline",
                      bot->GetName(), trash->GetGUID().ToString(), bot->GetExactDist(trash));
        return false;
    }

    return true;
}

bool DungeonClearRoomTrashTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Milestone 3: room-aggro is migrating onto the conditional-event path
    // (DungeonClearEventDueTrigger / DcRunEventAction at relevance 31). For a map
    // that already has a room-aggro pre-clear event authored, stand this legacy
    // rel-26 driver down so the two never both drive — the event path owns it.
    // Maps without an event row keep this path until they get one.
    if (DungeonEventRegistry::HasRoomAggroEvent(map->GetId()))
        return false;

    // Only at a flagged boss with room trash still up.
    if (!DcTargeting::IsRoomClearActive(bot, context))
        return false;

    // When pull-to-camp is in effect for this pack, the higher-priority pull
    // pipeline (relevance 35) owns the room clear so it honours the advanced/
    // dynamic pull type. This Leeroy room-clear is the Off / Dynamic-chose-Leeroy
    // path; stand down whenever the behavioural pull bool is set. The patrol-wait
    // hold (decision == 3) is pull-mode-off but likewise pull-pipeline-owned, so
    // stand down there too rather than Leeroy a room mob mid-wait.
    if (AI_VALUE(bool, "dungeon clear pull mode current") ||
        AI_VALUE(DcPullContext&, "dungeon clear pull context").decision == 3u)
        return false;

    // Same between-pulls gating the other engage triggers use (loot, party
    // catch-up, rest) so the room is cleared one careful pull at a time.
    if (!IsBetweenPullsReady(bot, context))
        return false;

    return DcTargeting::NearestRoomTrash(bot, context) != nullptr;
}

bool DungeonClearPartyDiedTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
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
    if (!IsEnabled(context, bot))
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
    if (!IsEnabled(context, bot))
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
    if (!IsEnabled(context, bot))
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

bool DungeonClearDoorReopenedTrigger::IsActive()
{
    if (!bot || bot->isDead())
        return false;
    // Only meaningful while THIS bot's own run is paused for a door (the leader's
    // paused flag). A manual `dc pause` leaves the paused-door GUID empty, so
    // opening some unrelated door never auto-resumes a hand-held pause.
    if (!AI_VALUE(bool, "dungeon clear enabled") || !AI_VALUE(bool, "dungeon clear paused"))
        return false;

    ObjectGuid const doorGuid = AI_VALUE(ObjectGuid, "dungeon clear paused door");
    if (doorGuid.IsEmpty())
        return false;

    // Resume once the blocker is gone: the door now reads OPEN (a player opened
    // it), or it despawned / its grid unloaded (GetGameObject returns null). In
    // either case the corridor is no longer held — IsDoorClosed treats a null GO
    // as not-closed, so the single test covers both.
    GameObject* door = botAI->GetGameObject(doorGuid);
    return !DcEngageGeometry::IsDoorClosed(door);
}

bool DungeonClearFollowTankTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;

    // In advanced-pull mode the party HOLDS and leapfrogs camp-to-camp instead of
    // following the tank (see DungeonClearHoldAtCampTrigger) — following would
    // trail the tank forward into every pull, which is the "party piles onto the
    // pull" chaos. Stand down whenever a camp is established so hold-at-camp owns
    // the follower; it also tears down any leftover MoveFollow generator itself.
    {
        Position camp;
        bool passive = false;
        if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
            return false;
    }

    // Redirect every non-leader bot to the leader — non-tanks AND non-leader
    // (off-)tanks in a raid. The leader resolves "party tank" to itself, so the
    // `tank != bot` guard below excludes only the leader (it doesn't follow
    // itself); a non-leader tank follows the leader out of combat just like any
    // DPS, then peels off to help tank once it enters combat (checked above).
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

namespace
{
    // Shared gate for the rest-target triggers: this bot is a living, out-of-
    // combat member of an active DC run (the cross-bot party-tank value is
    // non-null only while the leader's clear runs and is unpaused), and the run
    // has set a non-zero rest target for the given key. Returns the target % (0
    // when the trigger should stay inert).
    uint32 RestTargetIfActive(Player* bot, AiObjectContext* context, char const* key)
    {
        if (!bot || bot->isDead() || bot->IsInCombat())
            return 0;
        if (!AI_VALUE(Player*, "dungeon clear party tank"))
            return 0;
        return DcSettings::GetUInt(bot, key);
    }
}

bool DungeonClearNeedsDrinkTrigger::IsActive()
{
    uint32 const target = RestTargetIfActive(bot, context, "RestManaPct");
    if (target == 0)
        return false;
    // Non-mana classes (warriors/rogues) never drink.
    if (bot->GetMaxPower(POWER_MANA) == 0)
        return false;
    return bot->GetPowerPct(POWER_MANA) < static_cast<float>(target);
}

bool DungeonClearNeedsEatTrigger::IsActive()
{
    uint32 const target = RestTargetIfActive(bot, context, "RestHealthPct");
    if (target == 0)
        return false;
    return bot->GetHealthPct() < static_cast<float>(target);
}

bool DungeonClearPullTrigger::IsActive()
{
    if (!IsEnabled(context, bot))  // enabled, not paused, leader
        return false;
    if (!bot || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // `dungeon clear pull mode current` refreshes the Dynamic (pull setting == 2)
    // Leeroy/Advanced verdict for THIS tick and returns the behavioural pull-mode
    // bool. Reading it here (instead of mutating the bool as a side effect of this
    // IsActive() and hoping later readers run after us) keeps the verdict
    // order-independent: every reader — the engage/blocking-trash triggers and the
    // camp gates — consults the same value, so whichever runs first updates it.
    // No-op for Off/On, where DcPullAction owns the bool.
    bool const pullModeCurrent = AI_VALUE(bool, "dungeon clear pull mode current");
    // decision == 3 is the patrol-wait HOLD: pull mode is off-but-held, so the
    // behavioural bool reads false, but the pull action must still run to halt the
    // tank at commit range while it waits the patrol out. Keep the trigger live in
    // that state too. (Reading pull mode current FIRST runs the governor that may
    // set decision == 3 this tick.)
    bool const patrolWaiting =
        AI_VALUE(DcPullContext&, "dungeon clear pull context").decision == 3u;
    if (!pullModeCurrent && !patrolWaiting)
        return false;

    uint32 const phase = static_cast<uint32>(AI_VALUE(DcPullContext&, "dungeon clear pull context").phase);

    // Mid-pull pre-combat (Forming/Advancing) and the post-fight Engage cleanup
    // run on this non-combat engine, but only while out of combat — the instant
    // the tank aggros, control passes to the combat maneuver trigger.
    if (phase != static_cast<uint32>(DcPullPhase::Idle))
        return !bot->IsInCombat();

    // Idle: decide whether to START a pull. Must be out of combat, not at the
    // boss (trash-only), and the between-pulls gates clear (loot/rest/catch-up).
    if (bot->IsInCombat())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;
    // Parked at a pending-summon boss (e.g. RFD's gong -> Tuten'kash): do NOT
    // start a pull. The tank must hold at the anchor to run the event (ring,
    // fight the wave, ring again); a dynamic pull here latches a distant trash
    // pack and tows the tank out of the room, and it never returns to finish the
    // rings. The at-boss stand-down below only covers ~engage range, which the
    // wave fight pushes the tank past — this holds the wider event radius.
    if (DcTargeting::IsHoldingForSummonEvent(bot, context, *next))
        return false;
    // Normally the pull pipeline stands down at the boss (the at-boss engage owns
    // it). The one exception is a room-wide-aggro boss with room trash still up:
    // there the pull pipeline is what clears that room (honouring advanced/dynamic
    // pull), so it must stay live at the boss until the room is clear.
    if (DcTickMemoAccess::AtBossEngage(bot, context, *next) &&
        !DcTargeting::IsRoomClearActive(bot, context))
        return false;
    if (!IsBetweenPullsReady(bot, context))
        return false;

    Unit* trash = DcTargeting::GetPullTarget(botAI);
    if (!trash)
        return false;

    // Don't loop on a pack a previous pull gave up on — let engage-trash walk in.
    if (AI_VALUE(DcPullContext&, "dungeon clear pull context").abortTarget == trash->GetGUID())
    {
        DC_PULL_DEBUG("[DC:{}] pull trigger: target {} is the abort target -> defer "
                      "to normal engage", bot->GetName(), trash->GetGUID().ToString());
        return false;
    }

    // We fire even while the pack is still beyond run-in reach (the pull-target scan
    // already caps the look-ahead at ~35yd). The action does NOT commit yet — its
    // Idle branch yields to Advance to glide the tank closer — but running it now
    // lets it PUBLISH a prospective camp each glide tick so the party walks up to
    // the camp IN PARALLEL with the tank's approach instead of waiting for the
    // tank to arrive and only then trudging forward. The blocking-trash trigger
    // still stands down in pull mode, so Advance keeps driving the glide.
    float const toTrash = bot->GetExactDist2d(trash);
    float const commitRange =
        DcEngageGeometry::PullCommitRange(bot, trash, DC_PULL_START_RANGE);
    DC_PULL_DEBUG("[DC:{}] pull trigger: active — target {} at {:.1f}yd (commit {:.1f}, {})",
                  bot->GetName(), trash->GetGUID().ToString(), toTrash, commitRange,
                  toTrash > commitRange ? "glide + advance party camp" : "commit");
    return true;
}

bool DungeonClearPullManeuverTrigger::IsActive()
{
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    // `paused` is deliberately NOT checked here: pause is a soft stop, and
    // abandoning the tank mid-drag with a live pack on it is worse than letting
    // the drag finish. The follower side agrees — GetLeaderPullInfo /
    // GetLeaderCampHold keep the camp hold alive through a pause while a
    // maneuver phase is holding — so the party stays pinned at camp until the
    // drag resolves to Engage, after which the normal paused gates hold the run.
    if (!AI_VALUE(bool, "dungeon clear enabled") || !AI_VALUE(bool, "dungeon clear pull mode"))
        return false;
    if (!DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;

    uint32 const phase = static_cast<uint32>(AI_VALUE(DcPullContext&, "dungeon clear pull context").phase);
    // Forming is included so combat taken DURING the pre-run-in retreat/dwell is
    // not a dead zone: the non-combat pull trigger goes silent the instant the
    // tank is in combat (it returns !IsInCombat for any non-Idle phase), and if
    // the maneuver didn't pick Forming up here NOTHING would drive the pull —
    // the party would stay passive while the tank solos until the mob dies. With
    // Forming handled, an early aggro hands straight to the drag-back maneuver.
    //
    // Idle is included for the SAME reason one phase earlier: while the tank is
    // merely scouting toward the next pack (phase Idle, not yet committed) a
    // patrol/add can aggro it. Without this the maneuver wouldn't fire and stock
    // combat would fight in place wherever the tank happened to be — the "starts
    // combat right here instead of moving back to camp" bug. With Idle handled,
    // any unplanned aggro also drags back to the held party at camp first.
    // (Engage is deliberately still excluded so the camp fight itself runs
    // normally — by then the phase is Engage, never Idle.)
    return phase == static_cast<uint32>(DcPullPhase::Idle) ||
           phase == static_cast<uint32>(DcPullPhase::Forming) ||
           phase == static_cast<uint32>(DcPullPhase::Advancing) ||
           phase == static_cast<uint32>(DcPullPhase::Returning);
}

bool DungeonClearHoldAtCampTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;
    // The leader drives the pull; it never holds at its own camp.
    if (DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;

    // Hold at camp throughout pull mode (NOT just mid-maneuver): in pull mode the
    // party leapfrogs camp-to-camp and never follows the tank, so this fires while
    // the tank is merely scouting between pulls too. Passive is applied by the
    // action only during the holding phases (see GetLeaderCampHold's passiveOut).
    Position camp;
    bool passive = false;
    return DcLeaderSignal::GetLeaderCampHold(bot, camp, passive);
}

bool DungeonClearHoldAtCampCombatTrigger::IsActive()
{
    // Combat-engine twin of DungeonClearHoldAtCampTrigger, but it fires ONLY
    // during the holding pull phases (passiveOut) — when the tank is tagging and
    // the party must stay pinned and passive across the combat boundary the pull
    // drags it over. During the camp fight (Engage) and any unplanned aggro while
    // scouting (Idle) it deliberately does NOT fire, so the party fights normally.
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    // The leader drives the pull; it never holds at its own camp.
    if (DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;

    Position camp;
    bool passive = false;
    if (!DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
        return false;
    return passive;
}

namespace
{
    // Gate for the COMBAT-side fight assist: this follower's leader is mid
    // fight AND the bot currently has NO line-of-sight target of its own. The
    // empty-attackers test is what makes the combat assist self-limiting: the
    // stock AttackersValue LOS-filters, so "attackers" is empty exactly while the
    // pack is out of sight (the bug) and becomes non-empty the moment movement
    // carries the bot back into sight — at which point this goes inert and the
    // bot's own combat rotation (NOT multiplier-suppressed in the combat engine)
    // owns the fight again. The NON-combat side deliberately does NOT use this gate
    // (see DungeonClearAssistCampTrigger).
    bool ShouldAssistCampFight(PlayerbotAI* botAI, Player* bot)
    {
        if (!DcLeaderSignal::IsLeaderFightAssistWanted(bot))
            return false;
        return botAI->GetAiObjectContext()
                   ->GetValue<GuidVector>("attackers")->Get().empty();
    }
}

bool DungeonClearAssistCampTrigger::IsActive()
{
    // Non-combat side: ANY follower still out of combat while the leader tank
    // fights must be driven in — NOT just the out-of-LOS ones. An idle follower
    // that DOES have line of sight to the fight cannot self-engage either: DC's
    // multiplier suppresses the stock proactive-engagement pickers ("attack
    // anything" / pull) for every follower while a clear is active, so without an
    // explicit push it just stands there watching. And a follower the fight never
    // touched (tank aggroed around a corner / past natural engage range) never
    // enters combat at all, so the combat-engine regroup/assist can't reach it.
    // The assist action force-targets the pack and SetInCombatWith()s the bot,
    // flipping it into the combat engine where its own rotation/heal logic
    // (un-suppressed there) takes over. Hence we gate on the leader's fight
    // alone, not on an empty attacker list. Covers the advanced-pull camp fight
    // AND every fight the camp machinery does not own (Leeroy/dynamic/boss);
    // defers to the camp hold during the passive pull phases.
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;
    return DcLeaderSignal::IsLeaderFightAssistWanted(bot);
}

bool DungeonClearAssistCampCombatTrigger::IsActive()
{
    // Combat side: a follower dragged into combat (group combat / stray hit) but
    // with the pack around a corner has an empty LOS attacker list and so idles
    // in the combat engine. Drive it into sight so stock combat can engage.
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    return ShouldAssistCampFight(botAI, bot);
}

bool DungeonClearLeaderAssistTrigger::IsActive()
{
    // Leader-side assist. All the gating (leader-only, out of combat, no own
    // target, a groupmate latched in combat, not mid-drag) lives in the predicate.
    return DcLeaderSignal::IsLeaderShouldAssistFight(bot);
}

bool DungeonClearRegroupCombatTrigger::IsActive()
{
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;

    // Feature toggle.
    if (!DcSettings::GetBool(bot, "CombatRegroup"))
        return false;

    // The elected leader tank, non-null only while its clear is active and
    // unpaused. This both gates the feature on an active run and is the bot we
    // regroup on. The leader itself never regroups on anyone — it drives.
    Player* tank = AI_VALUE(Player*, "dungeon clear party tank");
    if (!tank || tank == bot || tank->GetMapId() != bot->GetMapId())
        return false;

    // Hand all advanced-pull camp positioning to the camp/assist actions: while
    // the party is held PASSIVE at a camp (Forming/Advancing/Returning) it must
    // not chase the moving tank or it would break the pull. Once released (Engage,
    // passive == false) or in any non-camp fight, regroup runs — including the
    // healer-LOS case the camp-fight assist (which only engages the pack) misses.
    Position camp;
    bool passive = false;
    if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive) && passive)
        return false;

    float const dist = bot->GetExactDist2d(tank);
    float const maxLeash = DcSettings::GetFloat(bot, "CombatRegroupDistance");

    // Any follower closes back in once it drifts past the leash. A healer ALSO
    // closes whenever it loses line of sight to the tank — even inside the leash —
    // because without it the heal lands on nothing and the party dies. Melee/ranged
    // DPS out of LOS of the tank but in range of their own target are left alone.
    if (dist > maxLeash)
        return true;
    if (PlayerbotAI::IsHeal(bot) && !bot->IsWithinLOSInMap(tank))
        return true;
    return false;
}

bool DungeonClearFilterLootTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;
    // Fires on EVERY member of a paused run — the leader AND its followers (the
    // helper resolves the run owner cross-context, so it is true for the whole
    // party while paused). Followers are the point: they never set `enabled` and,
    // while paused, "dungeon clear party tank" goes null so the follow-tank
    // action that runs their inline loot filter stops firing — they revert to
    // the stock loot pipeline and grab below-floor junk, which keeps
    // IsAnyPartyMemberLooting true and stalls the tank. Covers ONLY the paused
    // gap: during an active run the inline filters in advance/follow-tank already
    // run. `dc off` clears `enabled`, handing loot fully back to stock.
    return DcLeaderSignal::IsInPausedDungeonClearRun(bot);
}

bool DungeonClearLootRollPendingTrigger::IsActive()
{
    if (!DcSettings::GetBool(bot, "BetterLootRolling"))
        return false;

    // Self-bot: the vote is the human's to cast (BetterLootRollAction casts
    // none), so an open window must not keep the trigger hot.
    if (botAI->IsRealPlayer())
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (Roll* roll : group->GetRolls())
    {
        auto voteItr = roll->playerVote.find(bot->GetGUID());
        if (voteItr == roll->playerVote.end() || voteItr->second != NOT_EMITED_YET)
            continue;

        // Mirror the action's only no-vote path: it skips a roll whose item
        // template doesn't resolve, so such a roll must not fire the trigger
        // every tick forever.
        if (sObjectMgr->GetItemTemplate(roll->itemid))
            return true;
    }

    return false;
}
