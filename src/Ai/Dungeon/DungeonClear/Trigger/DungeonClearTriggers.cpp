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
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
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

    // Advanced-pull commit range: a pull is only STARTED once its target pack is
    // within this distance, so the camp (stamped at the tank's spot) stays close
    // enough that the run-in reaches the pack and the drag-back is short. The
    // corridor scan sees packs out to ~35yd, so without this the camp lands in
    // the run-in's overshoot dead band and every pull after the first whiffs.
    // Must stay aligned with DC_PULL_START_RANGE in DungeonClearActions.cpp
    // (commit outside the pack's aggro radius so the tank Forms and the party sets
    // at camp before the tag, instead of face-pulling mid-glide).
    constexpr float DC_PULL_START_RANGE = 26.0f;

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
    //
    // The driving ladder also runs ONLY on the elected leader. In a raid several
    // tanks can have the flag set, but exactly one drives — the others follow it
    // like any other member (see DungeonClearFollowTankTrigger). Only the leader
    // ever reaches the leadership scan: followers don't set `enabled`, so they
    // short-circuit on the first check. See DungeonClearUtil::FindLeaderTank.
    bool IsEnabled(AiObjectContext* context, Player* bot)
    {
        if (!AI_VALUE(bool, "dungeon clear enabled") || AI_VALUE(bool, "dungeon clear paused"))
            return false;
        return DungeonClearUtil::IsDungeonClearLeader(bot);
    }

    bool IsBetweenPullsReady(Player* bot, AiObjectContext* context)
    {
        if (AI_VALUE(bool, "has available loot"))
            return false;
        float maxSpread = sConfigMgr->GetOption<float>(
            "DungeonClear.PartyMaxSpread", DC_PARTY_MAX_SPREAD_DEFAULT);
        // In advanced-pull mode the party deliberately holds back at the camp while
        // the tank scouts ahead, so a spread check measured against the TANK would
        // never pass once the tank moves out — and the tank would stop pulling
        // after the first camp. Drop the spread requirement here; the Forming
        // party-set gate ensures the party is actually at the (new) camp before the
        // tag, and the rest (HP/mana) check below still holds the pull until the
        // party has recovered at camp.
        if (AI_VALUE(bool, "dungeon clear pull mode current"))
            maxSpread = 100000.0f;
        return DungeonClearUtil::IsPartyReady(bot, DungeonClearUtil::RestMinHpPct(bot),
                                              DungeonClearUtil::RestMinMpPct(bot), maxSpread);
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

    // Close enough AND on the boss's own floor (not just 3D-near while passing
    // under an upper-floor boss). See IsAtBossEngage.
    if (!DungeonClearUtil::IsAtBossEngage(bot, context, *next, DC_ENGAGE_RANGE))
        return false;

    // A closed door between us and the boss means it's BEYOND it and not actually
    // reachable yet — even when it's within straight-line engage range (a boss or
    // its pack right behind the door). Without this, IsAtBossEngage (a pure
    // distance+floor test, door-blind) fires the at-boss engage, which outranks
    // door-blocked (30 vs 22) and bee-lines the tank onto/through the door. Stand
    // down so door-blocked parks at its stand-off; re-evaluates the instant the
    // door opens. Checked fresh, not via the 500ms-cached blocking-door value,
    // which can still read empty the moment the boss first comes into range.
    Creature* const liveBoss = DungeonClearUtil::GetLiveBoss(bot, context, next->entry);
    float const bx = liveBoss ? liveBoss->GetPositionX() : next->x;
    float const by = liveBoss ? liveBoss->GetPositionY() : next->y;
    float const bz = liveBoss ? liveBoss->GetPositionZ() : next->z;
    if (DungeonClearUtil::ClosedDoorBetween(bot, bx, by, bz))
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
    if (DungeonClearUtil::IsAtBossEngage(bot, context, *next, DC_ENGAGE_RANGE))
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
            trash = DungeonClearUtil::FindBlockingTrashOnPath(
                bot, path.segments, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
        }
        // No usable long-path cache — fall back to a single-shot corridor
        // computed inline so the trigger stays live in degraded conditions.
        else
        {
            Movement::PointsArray corridor;
            if (DungeonClearUtil::ComputeCorridor(bot, next->x, next->y, next->z, corridor))
                trash = DungeonClearUtil::FindBlockingTrashCorridor(
                    bot, corridor, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
            else
                trash = DungeonClearUtil::FindBlockingTrash(
                    bot, *next, DC_TRASH_CONE_RANGE, DC_TRASH_CONE_HALF_ANGLE, candidates);
        }
    }
    else
        trash = DungeonClearUtil::FindBlockingTrash(
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
    if (DungeonClearUtil::ClosedDoorBetween(bot, trash->GetPositionX(),
                                            trash->GetPositionY(), trash->GetPositionZ()))
    {
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] blocking-trash: vetoed pack {} ({:.1f}yd) — closed door "
                  "between us and it on our floor",
                  bot->GetName(), trash->GetGUID().ToString(), bot->GetExactDist(trash));
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
    return !DungeonClearUtil::IsDoorClosed(door);
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
        if (DungeonClearUtil::GetLeaderCampHold(bot, camp, passive))
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
    if (!AI_VALUE(bool, "dungeon clear pull mode current"))
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
    if (DungeonClearUtil::IsAtBossEngage(bot, context, *next, DC_ENGAGE_RANGE))
        return false;
    if (!IsBetweenPullsReady(bot, context))
        return false;

    Unit* trash = DungeonClearUtil::FindPullTarget(botAI, *next);
    if (!trash)
        return false;

    // Don't loop on a pack a previous pull gave up on — let engage-trash walk in.
    if (AI_VALUE(DcPullContext&, "dungeon clear pull context").abortTarget == trash->GetGUID())
    {
        DC_PULL_DEBUG("[DC:{}] pull trigger: target {} is the abort target -> defer "
                      "to normal engage", bot->GetName(), trash->GetGUID().ToString());
        return false;
    }

    // We fire even while the pack is still beyond run-in reach (FindPullTarget
    // already caps the look-ahead at ~35yd). The action does NOT commit yet — its
    // Idle branch yields to Advance to glide the tank closer — but running it now
    // lets it PUBLISH a prospective camp each glide tick so the party walks up to
    // the camp IN PARALLEL with the tank's approach instead of waiting for the
    // tank to arrive and only then trudging forward. The blocking-trash trigger
    // still stands down in pull mode, so Advance keeps driving the glide.
    float const toTrash = bot->GetExactDist2d(trash);
    float const commitRange =
        DungeonClearUtil::PullCommitRange(bot, trash, DC_PULL_START_RANGE);
    DC_PULL_DEBUG("[DC:{}] pull trigger: active — target {} at {:.1f}yd (commit {:.1f}, {})",
                  bot->GetName(), trash->GetGUID().ToString(), toTrash, commitRange,
                  toTrash > commitRange ? "glide + advance party camp" : "commit");
    return true;
}

bool DungeonClearPullManeuverTrigger::IsActive()
{
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    if (!AI_VALUE(bool, "dungeon clear enabled") || !AI_VALUE(bool, "dungeon clear pull mode"))
        return false;
    if (!DungeonClearUtil::IsDungeonClearLeader(bot))
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
    if (DungeonClearUtil::IsDungeonClearLeader(bot))
        return false;

    // Hold at camp throughout pull mode (NOT just mid-maneuver): in pull mode the
    // party leapfrogs camp-to-camp and never follows the tank, so this fires while
    // the tank is merely scouting between pulls too. Passive is applied by the
    // action only during the holding phases (see GetLeaderCampHold's passiveOut).
    Position camp;
    bool passive = false;
    return DungeonClearUtil::GetLeaderCampHold(bot, camp, passive);
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
    if (DungeonClearUtil::IsDungeonClearLeader(bot))
        return false;

    Position camp;
    bool passive = false;
    if (!DungeonClearUtil::GetLeaderCampHold(bot, camp, passive))
        return false;
    return passive;
}

namespace
{
    // Gate for the COMBAT-side camp-fight assist: this follower's leader is mid
    // camp-fight AND the bot currently has NO line-of-sight target of its own. The
    // empty-attackers test is what makes the combat assist self-limiting: the
    // stock AttackersValue LOS-filters, so "attackers" is empty exactly while the
    // pack is out of sight (the bug) and becomes non-empty the moment movement
    // carries the bot back into sight — at which point this goes inert and the
    // bot's own combat rotation (NOT multiplier-suppressed in the combat engine)
    // owns the fight again. The NON-combat side deliberately does NOT use this gate
    // (see DungeonClearAssistCampTrigger).
    bool ShouldAssistCampFight(PlayerbotAI* botAI, Player* bot)
    {
        if (!DungeonClearUtil::IsLeaderCampFightActive(bot))
            return false;
        return botAI->GetAiObjectContext()
                   ->GetValue<GuidVector>("attackers")->Get().empty();
    }
}

bool DungeonClearAssistCampTrigger::IsActive()
{
    // Non-combat side: ANY follower still out of combat while the tank fights the
    // camp pack must be driven in — NOT just the out-of-LOS ones. An idle follower
    // that DOES have line of sight to the pack cannot self-engage either: DC's
    // multiplier suppresses the stock proactive-engagement pickers ("attack
    // anything" / pull) for every follower while a clear is active, so without an
    // explicit push it just stands at camp watching the fight. The assist action
    // force-targets the pack and SetInCombatWith()s the bot, flipping it into the
    // combat engine where its own rotation/heal logic (un-suppressed there) takes
    // over. Hence we gate on the camp fight alone, not on an empty attacker list.
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;
    return DungeonClearUtil::IsLeaderCampFightActive(bot);
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
    return DungeonClearUtil::IsInPausedDungeonClearRun(bot);
}
