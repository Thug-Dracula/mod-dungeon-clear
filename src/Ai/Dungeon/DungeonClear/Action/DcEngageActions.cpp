/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearActions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Position.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproach.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproachIo.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDoorPolicy.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPathWorker.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/StridedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/SwimPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearStateValues.h"
#include "Playerbots.h"
#include "DcActionShared.h"

using namespace DcActionShared;

namespace
{

    // Shared tuning constants (DC_ENGAGE_RANGE, the cone scan range/angle,
    // DC_USE_CORRIDOR_SCAN, DC_CORRIDOR_*,
    // DC_PULL_START_RANGE) now live in DungeonClearTuning.h, shared with the
    // trigger ladder so the two layers cannot drift. The old per-context cone
    // names (DC_ENGAGE_CONE_RANGE / DC_ENGAGE_CONE_HALF_ANGLE) are the trash-cone
    // scan; use DC_TRASH_CONE_RANGE / DC_TRASH_CONE_HALF_ANGLE.

    // Any hostile already inside this radius of the bot will pull as the tank
    // moves, regardless of whether it sits on the corridor line. The engage
    // selector hits the closest such mob first so the tank never runs past an
    // adjacent pack to reach a farther on-corridor target.
    constexpr float DC_PROXIMITY_ENGAGE_RANGE = 20.0f;

    // Stand-off before a blocking door, as distance TRAVELLED ALONG the path to
    // where the route first reaches the doorway (see
    // DcEngageGeometry::DistAlongPathToClosedDoor) — NOT straight-line to the
    // door's GO origin, which can sit past the gap and made the walk-in glide
    // straight through it. The door-blocked action parks when the remaining
    // along-path travel to the door drops to this, every tick before the glide is
    // re-issued, so the tank halts on the near side of the doorway.
    constexpr float DC_DOOR_STOP_DISTANCE = 10.0f;

    // Once parked at a blocking door, open it with GameObject::Use (the exact
    // path a client right-click takes) — but ONLY when the bot is actually
    // entitled to, as decided by BotCanOpenDoorLikePlayer (a faithful mirror
    // of the core's lock adjudication; see DcDoorPolicy.h for the rules). The
    // raw GameObject::Use door branch toggles the GO state with no lock and no
    // script/event check, so calling it on the wrong door desyncs the
    // encounter; the entitlement gate is what keeps us honest. Lock-free
    // (script/encounter) doors are left shut and we wait for the human. Flip
    // to false to never auto-open anything.
    constexpr bool DC_ATTEMPT_DOOR_OPEN = true;

    // Minimum gap between Use() clicks on the same door. Auto-closing gates
    // (door.autoCloseTime — Strat's King's Square Gate re-shuts 3000ms after
    // opening) must be re-clicked when they shut again, but never spammed
    // every AI tick while one Use is still swinging the door open.
    constexpr uint32 DC_DOOR_REUSE_MS = 2500;

    // Never Use() a door the bot isn't actually standing at. GameObject::Use
    // has NO server-side range check, so a mis-flagged far door (stale value /
    // phantom blocker) would otherwise swing open from across the map — a
    // silent event desync no real player could cause. Generous vs. the ~5yd
    // client interact range because parking happens along-path at up to
    // DC_DOOR_STOP_DISTANCE before the 8yd door band, and a wide gate's GO
    // origin (its hinge) sits yards off the walkable centerline — legitimate
    // parks measure up to ~20yd from the origin. The failure class this guards
    // against is 50yd+, so room-local is the right cut.
    constexpr float DC_DOOR_USE_RANGE = 25.0f;

    // Observation-gap re-arm for the Blocked-state watchdog (DcApproachState::
    // doorStall*). A stall older than this with no fresh observation is treated
    // as ended — the door opened or the run moved on — so the next stall on the
    // same door starts a fresh DungeonClear.DoorBlockedTimeout window instead
    // of inheriting stale accrual. Deliberately LONGER than door.autoCloseTime
    // cycles (~3s shut/open on auto-closing gates): a bot livelocked clicking a
    // gate open and watching it re-shut keeps accruing across cycles and still
    // times out into the pause.
    constexpr uint32 DC_DOOR_STALL_REARM_MS = 10000;


    // Per-approach counter resets now live as named subset methods on the owning
    // struct — DcApproachState::OnEnteredEngageRange (clear the dead-end
    // escalation counter + direct-pursuit give-up latch) and ::OnBossChange (wipe
    // every per-approach counter/latch + the position sentinel). The sticky
    // engage-trash target is a separate value, reset alongside OnBossChange at the
    // call site in Execute.

    // True only when the bot is genuinely ENTITLED to open this door — i.e. a
    // human player at this keyboard could open it by clicking. The slot
    // adjudication mirrors the core's Spell::CanOpenLock and lives in
    // DcDoorPolicy::CanOpenSlots (pure, unit-tested); see that header for the
    // full rules. The short version:
    //
    //   - lockId == 0 -> NEVER. A closed lock-free door in an instance is
    //     script/encounter-controlled (the Uldaman Ironaya seal and friends
    //     carry no lock and no NOT_SELECTABLE flag until their event runs).
    //     Clickable doors always reference a Lock.dbc row, even an empty one.
    //   - An empty lock (a row with no typed slots, e.g. Deadmines lock 85) or
    //     a bare-hands locktype slot (Quick/Slow Open, e.g. lock 86) opens for
    //     anyone — these were wrongly refused before, which is why the tank
    //     paused at every plain Deadmines door.
    //   - Key items (Scarlet Key, Key to the City) and lockpicking open their
    //     locks exactly as a player would.
    //   - GO_FLAG_LOCKED suppresses the bare-hands slots: flagged gates demand
    //     the real key/skill (Strat's King's Square Gate carries a Quick Open
    //     slot yet requires the Key to the City).
    //
    // This remains the gate that keeps the tank from force-opening doors it
    // has no business opening: GameObject::Use's door branch toggles the GO
    // state with NO lock and NO script/event check, so it must only ever be
    // called on a door this returns true for.
    bool BotCanOpenDoorLikePlayer(Player* bot, GameObject* go)
    {
        if (!bot || !go)
            return false;
        GameObjectTemplate const* info = go->GetGOInfo();
        if (!info || info->type != GAMEOBJECT_TYPE_DOOR)
            return false;
        // Not-selectable / can't-interact doors are driven purely by the
        // instance/boss scripting (encounter gates, "kill the boss" doors).
        // A player can't click them; never force them.
        if (go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) ||
            go->HasGameObjectFlag(GO_FLAG_INTERACT_COND))
            return false;

        uint32 const lockId = info->GetLockId();
        if (!lockId)
            return false;           // lock-free: script/encounter door

        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return false;           // unknown lock — don't force it open

        DcDoorPolicy::LockSlot slots[DcDoorPolicy::LOCK_SLOT_COUNT];
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            slots[i].keyType = lock->Type[i];
            slots[i].index = lock->Index[i];
            slots[i].requiredSkill = lock->Skill[i];
        }

        int32 const lockpick = bot->HasSkill(SKILL_LOCKPICKING)
                                   ? static_cast<int32>(bot->GetSkillValue(SKILL_LOCKPICKING))
                                   : -1;
        return DcDoorPolicy::CanOpenSlots(
            slots, DcDoorPolicy::LOCK_SLOT_COUNT,
            go->HasGameObjectFlag(GO_FLAG_LOCKED),
            // Keys aren't consumed by opening, so possession is the requirement.
            [bot](uint32 itemEntry) { return bot->HasItemCount(itemEntry, 1); },
            lockpick);
    }

    // Returns the stored sticky engage-trash target if it's still a valid pull
    // candidate. Nullptr means "re-pick from scratch".
    //
    // COMMIT-AND-HOLD. Release only on conditions that make this mob genuinely
    // no longer our target: gone, dead, off-map, no longer hostile, or evading
    // (reset and leashing back to spawn). It deliberately does NOT drop on
    // distance or on a per-tick reachability re-probe. Both of those fluctuate
    // as the bot walks and rounds corners, and re-picking on them flip-flops
    // the target between two nearby mobs — leaving the tank bouncing in place
    // between competing MoveTo commands, the exact bug this sticky exists to
    // prevent. (The old 25yd distance gate was narrower than the 35yd
    // corridor/cone pick range, so any mob picked in the 25-35yd band was
    // dropped the very next tick and re-picked — a built-in oscillation.)
    //
    // The two concerns those gates addressed are handled elsewhere without a
    // noisy per-tick check: the pick is corridor/cone- and level-reachability-
    // filtered at selection time (see Execute and FindNearestHostileWithin),
    // and "engage a nearer pack before running to a far mob" is the proximity
    // preempt in Execute, which commits to whatever it preempts to.
    Unit* ResolveStickyTrashTarget(Player* bot, ObjectGuid stickyGuid)
    {
        if (!bot || stickyGuid.IsEmpty())
            return nullptr;
        Unit* u = ObjectAccessor::GetUnit(*bot, stickyGuid);
        if (!u || !u->IsInWorld() || !u->IsAlive())
            return nullptr;
        if (u->GetMapId() != bot->GetMapId())
            return nullptr;
        if (!bot->IsHostileTo(u))
            return nullptr;
        // A creature that has reset and is leashing home can't be pulled where
        // it stands; give up the chase and let the selector re-pick. This is
        // the clean "stop sticking" signal that replaces the old distance gate.
        if (Creature* c = u->ToCreature())
            if (c->IsInEvadeMode())
                return nullptr;
        return u;
    }

    // True when `target` sits at the end of a single complete PathGenerator
    // route (PATHFIND_NORMAL). A long, winding route — the classic case being a
    // pack at the foot of a ramp the tank is standing atop — overruns
    // PathGenerator's hop cap and resolves only as PATHFIND_INCOMPLETE. The raw
    // MoveTo that EngageDirect issues can then only build the truncated prefix
    // of that route, which dead-ends against the geometry: the tank glides a
    // few yards (or not at all) and then sits in "clearing trash" forever,
    // never closing to aggro range. Such far targets must be approached via the
    // no-hop-cap long-path that Advance drives, so engage-trash yields to it
    // rather than bee-lining (see DungeonClearEngageTrashAction::Execute).
    bool IsDirectlyReachable(Player* bot, Unit* target)
    {
        if (!bot || !target)
            return false;
        PathGenerator gen(bot);
        gen.CalculatePath(target->GetPositionX(), target->GetPositionY(),
                          target->GetPositionZ(), /*forceDest*/ false);
        return gen.GetPathType() == PATHFIND_NORMAL;
    }

    // Closest valid hostile from `candidates` within `range` of the bot,
    // LOS-checked. Drives the proximity preempt in DungeonClearEngageTrashAction
    // so the tank engages a mob already inside its aggro bubble before running
    // to a farther on-corridor target. LOS is checked here because the
    // far-targets candidate list is built with LOS ignored.
    Unit* FindNearestHostileWithin(Player* bot, float range, GuidVector const& candidates)
    {
        if (!bot)
            return nullptr;
        Unit* best = nullptr;
        float bestDist = range;
        for (ObjectGuid guid : candidates)
        {
            Unit* u = ObjectAccessor::GetUnit(*bot, guid);
            if (!u || !u->IsAlive())
                continue;
            if (!bot->IsHostileTo(u))
                continue;
            float const dist = bot->GetDistance(u);
            if (dist >= bestDist)
                continue;
            if (!bot->IsWithinLOSInMap(u))
                continue;
            // A mob a floor above/below can sit inside the proximity bubble and
            // pass LOS through a railing/gap yet have no path — engaging it
            // wedges the tank. Require a real route before letting it preempt.
            if (!DcEngageGeometry::IsLevelReachable(bot, u))
                continue;
            best = u;
            bestDist = dist;
        }
        return best;
    }

}

bool DungeonClearEngageActionBase::EngageDirect(Unit* target)
{
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return false;

    bool const melee = botAI->IsMelee(bot);
    float const attackRange = melee
        ? (bot->GetCombatReach() + target->GetCombatReach() + 1.0f)
        : (botAI->GetRange("spell") - CONTACT_DISTANCE);
    float const distance = bot->GetExactDist(target);

    if (distance > attackRange)
    {
        // Room-aggro skirt: if a flagged boss's aggro sphere sits between us and
        // the target, detour AROUND it first instead of bee-lining through it and
        // waking the whole room mid-clear (the SM Cathedral / Mograine failure).
        // RoomAggroSkirtPoint is a no-op (nullopt) outside an active room clear, so
        // ordinary corridor engages keep their straight line. The detour leg is
        // NORMAL priority (combat may interrupt — we're not committed to a pull
        // yet) and is consumed each tick; the orbit emerges from per-tick re-aiming
        // and ends the moment the direct line clears. A detour that can't be pathed
        // falls through to the straight approach rather than freezing the clear.
        if (std::optional<Position> wp = RoomAggroSkirtPoint(target))
        {
            bool const moved = DcMoveTo(bot->GetMapId(), wp->GetPositionX(),
                                      wp->GetPositionY(), wp->GetPositionZ(),
                                      /*idle*/ false, /*react*/ false,
                                      /*normal_only*/ false, /*exact_waypoint*/ false,
                                      MovementPriority::MOVEMENT_NORMAL);
            if (moved || bot->isMoving() ||
                IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
                return true;
            // else: detour unwalkable — fall through to the direct approach.
        }

        // Optional: try a single class pull-spell opener before walking in.
        // Fire-and-forget — if the cast fails (out of range, on cooldown,
        // silenced) we just fall through to the walk; ResolvePullSpell already
        // dropped the opener entirely if the tank never trained it.
        // Suppress repeats while we're closing on the same target so we
        // don't spam casts each tick.
        if (DC_TRY_PULL_SPELL)
        {
            ObjectGuid const lastPullTarget =
                AI_VALUE(DcPullContext&, "dungeon clear pull context").tagTarget;
            if (lastPullTarget != target->GetGUID())
            {
                if (auto pick = ResolvePullSpell(botAI, bot))
                {
                    if (distance >= pick->minRange && distance <= pick->maxRange &&
                        bot->IsWithinLOSInMap(target))
                    {
                        bot->SetSelection(target->GetGUID());
                        if (botAI->CastSpell(pick->spellId, target))
                        {
                            context->GetValue<DcPullContext&>("dungeon clear pull context")
                                ->Get().tagTarget = target->GetGUID();
                            context->GetValue<Unit*>("current target")->Set(target);
                            // Don't change engine state yet — let combat
                            // get tagged naturally when the pull lands.
                        }
                    }
                }
            }
        }

        // Walk into range. We deliberately skip the pull pipeline's reach-pull
        // mechanism because its threshold arithmetic creates a dead zone
        // (too-far-to-pull, too-close-to-reach) that leaves the tank standing
        // right at the edge of aggro range.
        //
        // COMBAT priority only for the final approach: it can't be interrupted
        // by the bot's combat reflexes (see DungeonClearAdvanceAction), so on a
        // longer walk it makes the tank plow through any pack it aggros without
        // stopping. Past the close-approach band use NORMAL so combat interrupts
        // and the tank fights what it pulls en route.
        MovementPriority const prio =
            (distance <= attackRange + DC_COMBAT_APPROACH_RANGE)
                ? MovementPriority::MOVEMENT_COMBAT
                : MovementPriority::MOVEMENT_NORMAL;
        bool const moved = DcMoveTo(target->GetMapId(),
                                  target->GetPositionX(),
                                  target->GetPositionY(),
                                  target->GetPositionZ(),
                                  /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                  /*exact_waypoint*/ false, prio);

        // Decisive approach: commit to one continuous run into range instead of
        // the old stutter-creep. MoveTo returns false on a DUPLICATE move (same
        // destination while a prior glide is still in flight) — that means "the
        // run is already happening", NOT a failure. The old `return MoveTo(...)`
        // surfaced that false to the engine, which then fell through to the
        // lower-relevance Advance action; Advance saw us inside engage range and
        // called StopMoving — so every other tick killed the glide and the tank
        // crept in a few yards at a time. Treat an in-flight / freshly-queued
        // move as success (consume the tick, keep gliding); only a genuine
        // "couldn't move AND not moving" falls through so Advance's posStuck /
        // stall escalation can still catch a real wedge. Mirrors the direct-
        // pursuit branch in DungeonClearAdvanceAction.
        if (moved || bot->isMoving() || IsWaitingForLastMove(prio))
            return true;
        return false;
    }

    DcMovement::StopBot(bot, DcMovement::Stop::Soft);

    bot->SetSelection(target->GetGUID());
    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    context->GetValue<Unit*>("current target")->Set(target);
    bot->Attack(target, melee);
    // Non-aggressive ("yellow"-name, neutral) bosses won't aggro just because
    // the tank is standing in melee range, and bot->Attack() alone doesn't
    // reciprocate combat until a swing lands — which stock combat targeting can
    // drop the target before. Without a ranged pull, the proximity walk-in then
    // never actually engages. Force such a creature into combat with us so the
    // pull is deterministic. Scoped to non-hostile targets only: a "red"-name
    // hostile mob (IsHostileTo, reaction <= REP_HOSTILE) aggros on its own, so
    // we leave its natural pull untouched.
    if (Creature* creature = target->ToCreature())
        if (!bot->IsHostileTo(creature))
            creature->EngageWithTarget(bot);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
    return true;
}

std::optional<Position> DungeonClearEngageActionBase::RoomAggroSkirtPoint(Unit* target)
{
    if (!target)
        return std::nullopt;

    // Skirt whenever a room-aggro pre-clear is in progress for the NEXT boss —
    // independent of whether the tank has reached boss-engage range yet.
    //
    // The old gate (IsRoomClearActive) ALSO required IsAtBossEngage, i.e. the
    // skirt only armed once the tank was already AT the boss's aggro edge. But
    // the room-aggro pre-clear event engages the nearest trash from far out (its
    // condition has no distance gate — see EventConditionRegistry::RoomAggroPreClear),
    // so during the approach EngageDirect ran with the skirt OFF and bee-lined a
    // straight line that could cut clean through the boss's aggro sphere before
    // the skirt ever armed. That is the live SM Cathedral failure: the tank walks
    // into Commander Mograine's aggro range on its way to far-side Scarlet trash.
    // Gating the skirt on at-boss-engage disabled it during the very approach it
    // exists to protect.
    //
    // Gate instead on "next boss is a room-aggro boss with trash still up". The
    // chord/sphere test (NeedsRoomAggroSkirt) below no-ops when the straight
    // approach already clears the sphere, so ordinary far-out corridor walks pay
    // only a cheap registry + cached-value read and keep their direct line.
    std::optional<DungeonBossInfo> next =
        AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return std::nullopt;
    if (!RoomAggroRegistry::Find(bot->GetMapId(), next->entry))
        return std::nullopt;
    if (AI_VALUE(GuidVector, "dungeon clear room trash remaining").empty())
        return std::nullopt;

    Creature* boss = DcTargeting::GetLiveBoss(bot, context, next->entry);
    if (!boss)
        return std::nullopt;

    // Same sphere sizing as the room-trash value's exclusion: the boss's real
    // aggro range + both reaches + the aggro margin + the extra path padding.
    float const safeRadius = boss->GetAggroRange(bot) +
                             bot->GetCombatReach() + boss->GetCombatReach() +
                             DcSettings::GetFloat(bot, "AggroRangeMargin") +
                             DcSettings::GetFloat(bot, "RoomAggroPathPadding");

    std::optional<Position> wp = DcEngageGeometry::AggroSafeApproachPoint(
        bot, boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ(),
        safeRadius, target);
    if (wp)
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] room-clear: skirting {}'s aggro sphere (r={:.1f}) -> "
                  "detour ({:.1f}, {:.1f}) before approaching {}",
                  bot->GetName(), boss->GetName(), safeRadius,
                  wp->GetPositionX(), wp->GetPositionY(), target->GetName());
    return wp;
}

bool DungeonClearEngageActionBase::MoveToSkirtingRoomAggro(Unit* target,
                                                           MovementPriority prio)
{
    if (!target)
        return false;

    float dx = target->GetPositionX();
    float dy = target->GetPositionY();
    float dz = target->GetPositionZ();
    if (std::optional<Position> wp = RoomAggroSkirtPoint(target))
    {
        dx = wp->GetPositionX();
        dy = wp->GetPositionY();
        dz = wp->GetPositionZ();
    }

    bool const moved = DcMoveTo(target->GetMapId(), dx, dy, dz,
                              /*idle*/ false, /*react*/ false,
                              /*normal_only*/ false, /*exact_waypoint*/ false, prio);
    // Own the tick while the move is in flight (a duplicate-move returns false but
    // the bot is still gliding) — mirrors EngageDirect's walk-branch semantics.
    return moved || bot->isMoving() || IsWaitingForLastMove(prio);
}

// At the boss (close, on its floor) AND no anchored intermediate hops remain
// unresolved → stop walking and let the at-boss trigger fire the pull. With
// anchored routes the bot may be geometrically near the boss but still on the
// wrong side of a wall; we keep walking in that case until anchors are cleared.
// Likewise a boss one floor up is 3D-near but not atBoss, so the tank keeps
// walking the route up to its level instead of parking underneath.
bool DungeonClearEngageTrashAction::Execute(Event /*event*/)
{
    // Pause guard — same already-queued-action race as DungeonClearAdvanceAction.
    if (AI_VALUE(bool, "dungeon clear paused"))
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    ObjectGuid const stickyGuid =
        AI_VALUE(ObjectGuid, "dungeon clear engage trash target");
    Unit* sticky = ResolveStickyTrashTarget(bot, stickyGuid);

    // Prefer the wider DC-gated scan — it sees packs at the far end of
    // long dungeon corridors that fall outside the default 100yd
    // sightDistance cap. Falls back to `possible targets` when far-targets
    // is empty (e.g. very first tick, before its 500ms poll has run).
    GuidVector const& farTargets = AI_VALUE(GuidVector, "dungeon clear far targets");
    GuidVector const& possibleTargets = AI_VALUE(GuidVector, "possible targets");
    GuidVector const& candidates = farTargets.empty() ? possibleTargets : farTargets;

    Unit* fresh = nullptr;
    if (DC_USE_CORRIDOR_SCAN)
    {
        // Walk the cached long-path polyline. The polyline spans the full
        // chunked route, so blocking trash beyond a single PathGenerator
        // call is still detected. EnsureLongPath wasn't invoked here —
        // Advance refreshes it every tick; this read sees the same value.
        ChunkedPathfinder::Result const& path =
            AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
        if (path.reachable && !path.segments.empty())
        {
            fresh = DcTargeting::FindBlockingTrashOnPath(
                bot, path.segments, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
        }
        else
        {
            // No usable long-path cache — fall back to single-shot corridor.
            Movement::PointsArray corridor;
            if (DcEngageGeometry::ComputeCorridor(bot, next->x, next->y, next->z, corridor))
            {
                fresh = DcTargeting::FindBlockingTrashCorridor(
                    bot, corridor, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
            }
        }
    }
    if (!fresh)
    {
        // Final fallback: cone scan, same predicate as the trigger uses.
        // Keeps the engagement path live when path computation can't
        // produce any usable corridor (boss off-mesh, etc.).
        fresh = DcTargeting::FindBlockingTrash(
            bot, *next, DC_TRASH_CONE_RANGE, DC_TRASH_CONE_HALF_ANGLE, candidates);
    }

    // Proximity preempt: a hostile already inside the bot's aggro bubble will
    // pull as the tank moves regardless of whether it sits on the corridor
    // line, so engage the closest such mob first. This is the run-past-the-pack
    // fix — without it the selector commits to an on-line mob farther ahead and
    // the tank charges through any adjacent pack to reach it.
    Unit* const nearestThreat = FindNearestHostileWithin(bot, DC_PROXIMITY_ENGAGE_RANGE, candidates);

    // Target selection:
    //  1. If the current sticky is still inside the aggro bubble, keep it — it's
    //     close and route-stable. Otherwise take the closest in-bubble hostile.
    //  2. Nothing adjacent: fall back to the corridor/cone pick, preferring the
    //     sticky for route stability. The sticky target prevents the per-tick
    //     bounce between two roughly-equidistant corridor mobs. The one
    //     override: a fresh candidate already in combat with the party
    //     (healer/DPS pulled aggro) beats a quiet sticky — "more reason to
    //     engage, not less" — so the tank doesn't wander toward a quiet mob
    //     while the healer gets clawed.
    Unit* target = nullptr;
    if (sticky && bot->GetDistance(sticky) <= DC_PROXIMITY_ENGAGE_RANGE)
        target = sticky;
    else if (nearestThreat)
        target = nearestThreat;

    if (!target)
    {
        target = sticky;
        if (!target)
            target = fresh;
        else if (fresh && fresh != sticky && fresh->IsInCombat() && !sticky->IsInCombat())
            target = fresh;
    }

    if (!target)
    {
        if (!stickyGuid.IsEmpty())
            context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
        return false;
    }

    // Pin the chosen target so the next tick doesn't reconsider it.
    if (target->GetGUID() != stickyGuid)
        context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(target->GetGUID());

    // Far, long-route trash: a pack the navmesh can only reach via a winding
    // route that overruns PathGenerator's hop cap (the tank atop a ramp with
    // the pack at its foot, the route running all the way down and back over).
    // The raw MoveTo EngageDirect issues builds only the truncated
    // PATHFIND_INCOMPLETE prefix of that route and dead-ends against the
    // geometry — the tank freezes in "clearing trash" and never reaches aggro
    // range. Hand the approach to Advance's long-path (LongRangePathfinder, no
    // hop cap): it already routes toward the boss straight past this pack, so
    // returning false here yields the tick to it. As the tank descends and the
    // pack comes within a single-call NORMAL route, this action preempts again
    // (proximity/sticky) and pulls it. Gated on distance so ordinary
    // in-corridor pulls keep their direct bee-line; only genuinely far,
    // out-of-aggro targets are deferred.
    if (bot->GetDistance(target) > DC_ENGAGE_RANGE &&
        !IsDirectlyReachable(bot, target))
        return false;

    return EngageDirect(target);
}

bool DungeonClearEngageBossAction::Execute(Event /*event*/)
{
    // Pause guard — same already-queued-action race as DungeonClearAdvanceAction.
    if (AI_VALUE(bool, "dungeon clear paused"))
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    Creature* boss = DcTargeting::GetLiveBoss(bot, context, next->entry);
    if (!boss)
    {
        // A boss a pending event must SUMMON (RFD's gong -> Tuten'kash) isn't on
        // the map until the rings bring him up. Don't stall/"Blocked" — yield and
        // let the gong event (relevance 31) hold + ring; once summoned this is
        // non-null and we engage normally.
        if (DcTargeting::HasPendingSummonEvent(bot, context, next->entry))
            return false;
        StallDungeonClear(botAI,
            "Can't reach " + next->name + ": not spawned on this map. Use 'dc skip' to move to the next boss.");
        return false;
    }

    if (!EngageDirect(static_cast<Unit*>(boss)))
        return false;

    ClearStall(context);
    return true;
}

bool DungeonClearRoomClearAction::Execute(Event /*event*/)
{
    // Pause guard — same already-queued-action race as DungeonClearAdvanceAction.
    if (AI_VALUE(bool, "dungeon clear paused"))
        return false;

    // Nearest remaining room-trash unit (the value already excludes the boss,
    // other encounter bosses, anything inside the boss's aggro sphere, and
    // unreachable/door-blocked units). EngageDirect walks to ITS attack range —
    // not toward the boss — so the careful nearest-first clear never face-pulls
    // the boss.
    Unit* target = DcTargeting::NearestRoomTrash(bot, context);
    if (!target)
        return false;

    return EngageDirect(target);
}

bool DungeonClearClearStalledAction::Execute(Event /*event*/)
{
    Unit* target = DcTargeting::FindNearestReachableHostile(bot);
    if (!target)
    {
        // We're stalled with nothing left to kill. Leave the stall reason in
        // place so `dc status` reports it; the player can `dc skip` or `dc off`.
        std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
        std::string const target_name = next.has_value() ? next->name : "the next boss";
        StallDungeonClear(botAI,
            "Stuck near " + target_name + " and no reachable mobs left to clear. "
            "Use 'dc skip' to move on or 'dc off' to stop.");
        context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
        return false;
    }

    // Announce target on first selection. Suppress repeats while we're still
    // working on the same one.
    ObjectGuid const lastAnnounced =
        context->GetValue<ObjectGuid>("dungeon clear fallback target")->Get();
    if (lastAnnounced != target->GetGUID())
    {
        context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(target->GetGUID());
        DcStatusPublisher::SendAddonMessage(botAI, "CHAT\tClearing path \xe2\x80\x94 pulling " + std::string(target->GetName()) + ".");
    }

    // Don't clear the stall reason here — only a successful Advance does that.
    // If pathing to the boss is still blocked after this kill, we want the
    // stall trigger to fire again next tick.
    return EngageDirect(target);
}

bool DcObjectiveArriveAction::Execute(Event /*event*/)
{
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value() || next->kind != DungeonAnchorKind::Objective)
        return false;

    // Hold at the anchor while the event/hook runs — StopBot(Hold) cancels a
    // launched escort glide so the tank doesn't coast past the objective.
    DcMovement::StopBot(bot, DcMovement::Stop::Hold);

    // Prefer a declarative event (DungeonEventRegistry) when the anchor names
    // one; otherwise fall back to the legacy freeform hook (ObjectiveHookRegistry)
    // so existing objectives are unchanged. Both reduce to one drive outcome.
    EventDriveOutcome outcome = EventDriveOutcome::Completed;
    DungeonEvent const* ev =
        next->eventId ? DungeonEventRegistry::Find(next->mapId, next->eventId) : nullptr;
    if (ev)
    {
        auto& prog =
            context->GetValue<DungeonEventProgress&>("dungeon clear event progress")->Get();
        outcome = DungeonEventExecutor::Drive(bot, context, *ev, prog);
    }
    else
    {
        switch (ObjectiveHookRegistry::Run(next->onArriveHook, bot, context, *next))
        {
            case ObjectiveArriveResult::Running: outcome = EventDriveOutcome::Running; break;
            case ObjectiveArriveResult::Blocked: outcome = EventDriveOutcome::Stalled; break;
            case ObjectiveArriveResult::Done:
            default:                             outcome = EventDriveOutcome::Completed; break;
        }
    }

    if (outcome == EventDriveOutcome::Running)
    {
        // Event/hook still working — keep holding; it is driven again next tick.
        SetPhase(context, "objective");
        return true;
    }

    if (outcome == EventDriveOutcome::Stalled)
    {
        // The event needs the human (something the bot can't drive). Stall so
        // the player can sort it; they can also `dc skip` past the objective.
        StallDungeonClear(botAI, "I can't progress the event at " + next->name +
                                     " on my own. Sort it and I'll continue, or `dc skip`.");
        return true;
    }

    // Completed or Skipped: latch the objective complete so NextDungeonBossValue
    // advances and never re-targets it (objectives have no kill-bit to read).
    auto& cleared =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear cleared anchors")->Get();
    if (cleared.insert(next->entry).second)
    {
        ClearStall(context);
        DcStatusPublisher::SendAddonMessage(botAI, "CHAT\tReached " + next->name + " \xe2\x80\x94 continuing.");
        LOG_DEBUG("playerbots.dungeonclear",
                  "[dungeon-clear] {} reached objective '{}' (entry {}) — marked cleared",
                  bot->GetName(), next->name, next->entry);
    }
    SetPhase(context, "");
    return true;
}

bool DcRunEventAction::Execute(Event /*event*/)
{
    Map* map = bot ? bot->GetMap() : nullptr;
    if (!map)
        return false;

    DungeonEvent const* ev =
        DungeonEventExecutor::FindDueConditionalEvent(bot, context, map->GetId());
    if (!ev)
        return false;  // condition went false between trigger and action — stand down

    // Milestone 3: a room-aggro PRE-CLEAR event drives the engage pipeline
    // directly. The condition (room trash remains) gated us here; engage the
    // NEAREST room trash so the leader works the room from its edge inward (the
    // value already excludes the boss, its aggro sphere, and unreachable/door-
    // blocked units). Combat then takes over — the event trigger stands down once
    // the leader is in combat, the stock combat engine fights, and the assist-camp
    // rungs bring the followers in. When the room is clear NearestRoomTrash is
    // null and the condition reads false next tick, reopening the at-boss gate.
    // The event is never latched (repeatable per boss). IsBetweenPullsReady keeps
    // it to one careful pull at a time (loot / party catch-up / rest), matching
    // the legacy room-clear path this supersedes.
    if (DungeonEventRegistry::IsRoomAggroPreClear(*ev))
    {
        if (!IsBetweenPullsReady(bot, context))
        {
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] room-clear stand-down: not between-pulls ready "
                      "(loot/rest/catch-up) -> yielding (boss gate stays shut)",
                      bot->GetName());
            return false;
        }
        Unit* trash = DcTargeting::NearestRoomTrash(bot, context);
        if (!trash)
        {
            // The set is empty -> the room is considered clear and the at-boss
            // gate opens. If this fires while Scarlet trash is still alive, the
            // room-trash VALUE pruned them (see its exclusion-breakdown log) and
            // the boss is about to be pulled prematurely.
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] room-clear stand-down: NearestRoomTrash null -> room "
                     "deemed CLEAR, boss gate opening", bot->GetName());
            return false;  // room clear / nothing reachable — let the boss gate open
        }
        DcMovement::ResolveEscortConflict(bot);
        SetPhase(context, "room clear");

        // Engage the nearest room trash. EngageDirect's walk-in skirts the boss's
        // aggro sphere on its own (RoomAggroSkirtPoint) — a straight approach to a
        // pack on the far side of a centre-of-room boss (Mograine) would cut
        // through his aggro range and wake the whole room mid-clear. The room-trash
        // value excludes mobs INSIDE the sphere; the skirt handles the PATH across
        // it. Combat then takes over and the assist-camp rungs bring the followers.
        return EngageDirect(trash);
    }

    // Hold position while driving the event. ResolveEscortConflict only cancels a
    // launched escort glide (the coast-past from the advance ladder) — it leaves a
    // step's own intra-room MovePoint (HopTo) alone, so MoveTo/Gossip walk-ins
    // still work, unlike the StopMovingOnCurrentPos in StopBot(Hold).
    DcMovement::ResolveEscortConflict(bot);

    auto& prog =
        context->GetValue<DungeonEventProgress&>("dungeon clear conditional event progress")->Get();
    EventDriveOutcome const outcome = DungeonEventExecutor::Drive(bot, context, *ev, prog);

    char const* outcomeStr =
        outcome == EventDriveOutcome::Running   ? "running"
        : outcome == EventDriveOutcome::Completed ? "completed"
        : outcome == EventDriveOutcome::Stalled ? "stalled"
                                                : "skipped";
    LOG_DEBUG("playerbots.dungeonclear",
              "[DC:{}] run-event '{}' step {}/{} -> {}",
              bot->GetName(), ev->name, prog.stepIndex,
              static_cast<uint32>(ev->steps.size()), outcomeStr);

    if (outcome == EventDriveOutcome::Running)
    {
        SetPhase(context, "event");
        return true;
    }

    if (outcome == EventDriveOutcome::Stalled)
    {
        StallDungeonClear(botAI, "I can't progress the event '" + ev->name +
                                     "' on my own. Sort it and I'll continue, or `dc skip`.");
        return true;
    }

    // Completed or Skipped. A REPEATABLE event is never latched — it must fire
    // again the next time its condition reads true (e.g. the RFD gong, rung once
    // per wave). Its loop is ended by the condition itself going false for good
    // (Tuten'kash spawns), not by a one-shot latch. Clear any stall and let the
    // run proceed; the next due-check decides whether to repeat.
    if (ev->repeatable)
    {
        ClearStall(context);
        SetPhase(context, "");
        return true;
    }

    // Otherwise latch the event under its synthetic key so the trigger stops
    // re-firing it and the clear proceeds.
    auto& cleared =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear cleared anchors")->Get();
    if (cleared.insert(DungeonEventExecutor::ConditionalLatchKey(ev->id)).second)
    {
        ClearStall(context);
        DcStatusPublisher::SendAddonMessage(botAI, "CHAT\tEvent '" + ev->name + "' done \xe2\x80\x94 continuing.");
        LOG_DEBUG("playerbots.dungeonclear",
                  "[dungeon-clear] {} completed conditional event '{}' (id {}) — latched",
                  bot->GetName(), ev->name, ev->id);
    }
    SetPhase(context, "");
    return true;
}

bool DungeonClearDisableOnDeathAction::Execute(Event /*event*/)
{
    std::string deadName = "Someone";
    if (bot && bot->isDead())
    {
        deadName = bot->GetName();
    }
    else if (bot)
    {
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot)
                    continue;
                if (member->GetMapId() != bot->GetMapId())
                    continue;
                if (member->isDead())
                {
                    deadName = member->GetName();
                    break;
                }
            }
        }
    }

    DisableDungeonClear(botAI, deadName + " died \xe2\x80\x94 dungeon clear disabled. Type 'dc on' when ready to resume.");
    return true;
}

bool DungeonClearDisableOnClearedAction::Execute(Event /*event*/)
{
    DisableDungeonClear(botAI, "All bosses cleared!");
    return true;
}

bool DungeonClearDoorBlockedAction::Execute(Event event)
{
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    std::string const target = next.has_value() ? next->name : "the next boss";
    std::string const pauseReason =
        "A door blocks the path to " + target +
        " and I can't open it. Paused — open it and hit Resume.";
    std::string const timeoutReason =
        "The door to " + target +
        " won't open for me. Paused — open it (or finish its event) and hit Resume.";
    std::string const openingReason = "Opening the door to " + target + ".";

    ObjectGuid const doorGuid = AI_VALUE(ObjectGuid, "dungeon clear blocking door");
    GameObject* door = doorGuid.IsEmpty() ? nullptr : botAI->GetGameObject(doorGuid);

    // We've reached the door. Decide what a player in our shoes would do:
    //   - openable (plain door, or we hold the key / have the skill): interact
    //     with it the same way a client right-click does (GameObject::Use at
    //     interaction range), then hold for the tick — the door swings open,
    //     the blocking-door value empties next tick, and Advance resumes.
    //   - not openable (locked with no key/skill, or a script/encounter door):
    //     never touch the GO state — drop straight into pause mode. The player
    //     has to come solve the door anyway, and pause is exactly the right hold:
    //     the whole party stops driving and follows (see DungeonClearMultiplier)
    //     until the door is opened and the player hits Resume. This replaces the
    //     old bespoke "stalled at door" state, which was prone to the bot's stock
    //     AI later walking it straight through the opened door.
    // The Use() is throttled on the announced-reason transition so we don't
    // re-click a door every tick; when Advance resumes it clears the reason, so
    // a later re-close (autoclose doors) re-arms a fresh single attempt.
    auto parkAndStall = [&]()
    {
        DcMovement::StopBot(bot, DcMovement::Stop::Soft);

        // Script-only event doors (e.g. SFK's Courtyard Door 18895) wear a
        // plainly-clickable empty-lock template but the client only opens them
        // via their event — a generic Use() here desyncs the client and skips
        // the event. Never auto-open a listed door; fall through to the pause /
        // (preferably) let the dungeon-event free the prisoner that opens it.
        bool const canOpen = DC_ATTEMPT_DOOR_OPEN && door &&
                             !DcEventDoorRegistry::IsScriptOnly(door->GetEntry()) &&
                             BotCanOpenDoorLikePlayer(bot, door);
        bool timedOut = false;

        if (canOpen)
        {
            if (!DcEngageGeometry::IsDoorClosed(door))
            {
                // The door is open NOW (our click landed, or a player beat us
                // to it) — only the 500ms-cached blocking-door value hasn't
                // noticed yet. Drop the stall immediately so the status panel
                // stops reporting Blocked at an open door; Advance resumes the
                // moment the value refreshes empty.
                ClearStall(context);
                return true;
            }

            DcApproachState& doorAppr =
                context->GetValue<DcApproachState&>("dungeon clear approach state")->Get();
            uint32 const now = getMSTime();

            // Blocked-state watchdog. The entitlement above is template-level
            // truth and CAN be wrong (SFK's Arugal's Lair is an event door
            // wearing the same empty-lock-85 template as a plain clickable
            // Deadmines door), and the click gate below measures range to the
            // GO origin, which on wide gates sits outside DC_DOOR_USE_RANGE of
            // the path-side parking spot — either way the bot would work the
            // door forever. After DoorBlockedTimeout seconds with the door
            // still shut, give up and fall through to the auto-pause below:
            // the stashed GUID still auto-resumes the run the moment the door
            // really opens (event completes, or a player opens it).
            if (doorAppr.doorStallGuid != door->GetGUID() ||
                getMSTimeDiff(doorAppr.doorStallLastMs, now) >= DC_DOOR_STALL_REARM_MS)
            {
                doorAppr.doorStallGuid = door->GetGUID();
                doorAppr.doorStallSinceMs = now;
            }
            doorAppr.doorStallLastMs = now;
            timedOut = getMSTimeDiff(doorAppr.doorStallSinceMs, now) >=
                       DcSettings::GetUInt(bot, "DoorBlockedTimeout") * 1000;

            // A click is only legitimate from beside the door. Parked far from
            // it (mis-flag, or the walk-in hasn't closed the gap yet), hold
            // position and report instead of toggling a GO no player could
            // reach — Use() has no range check of its own.
            if (!timedOut && !bot->IsWithinDistInMap(door, DC_DOOR_USE_RANGE))
            {
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] door-blocked: entitled to open {} '{}' but "
                         "{:.1f}yd away (> {:.0f}yd) -> holding, not clicking",
                         bot->GetName(), door->GetGUID().ToString(),
                         door->GetName(), bot->GetExactDist(door), DC_DOOR_USE_RANGE);
                StallDungeonClear(botAI, openingReason);
                return true;
            }

            // Click whenever the door actually reads SHUT, on a per-door
            // cooldown — not the old "once per announced reason" latch. Gates
            // with door.autoCloseTime re-shut themselves seconds after opening
            // (King's Square Gate: 3s); under the latch, a gate that re-closed
            // before Advance ran never got a second click and the run sat
            // "Blocked" at a door it held the key for.
            if (!timedOut)
            {
                if (doorAppr.lastDoorUseGuid != door->GetGUID() ||
                    getMSTimeDiff(doorAppr.lastDoorUseMs, now) >= DC_DOOR_REUSE_MS)
                {
                    LOG_INFO("playerbots.dungeonclear",
                             "[DC:{}] door-blocked: opening {} '{}' as a player would (entitled)",
                             bot->GetName(), door->GetGUID().ToString(), door->GetName());
                    door->Use(bot);
                    doorAppr.lastDoorUseGuid = door->GetGUID();
                    doorAppr.lastDoorUseMs = now;
                }
                StallDungeonClear(botAI, openingReason);
                return true;
            }
        }

        // Can't open it ourselves — or timed out working a door we thought we
        // could — auto-pause and force the navigation dead.
        // Set the flag once on transition: the door-blocked trigger gates on
        // !paused, so it won't re-fire and re-announce every tick.
        if (!AI_VALUE(bool, "dungeon clear paused"))
        {
            context->GetValue<bool>("dungeon clear paused")->Set(true);
            // Record the cause so the status panel shows the door reason rather
            // than a generic hold (manual pause stamps its own reason instead).
            context->GetValue<std::string&>("dungeon clear pause reason")->Get() =
                "a closed door is blocking the path";
            // Stash THIS door's GUID so DungeonClearDoorReopenedTrigger can poll
            // it and auto-resume the instant a player opens it (door is non-null
            // here — the can-open branch above already required it). The null-door
            // fallback path below leaves this empty, so it simply stays manual.
            context->GetValue<ObjectGuid>("dungeon clear paused door")->Set(
                door ? door->GetGUID() : ObjectGuid::Empty);
            if (MotionMaster* mm = bot->GetMotionMaster())
                mm->Clear();
            bot->StopMovingOnCurrentPos();
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] door-blocked: {} {} -> auto-pausing",
                     bot->GetName(),
                     timedOut ? "timed out working" : "can't open",
                     door ? door->GetGUID().ToString() : "(none)");
            DcStatusPublisher::SendAddonMessage(
                botAI, "CHAT\t" + (timedOut ? timeoutReason : pauseReason));
            botAI->DoSpecificAction("dc status", event, true);
        }
        return true;
    };

    if (!door)
    {
        if (doorGuid.IsEmpty())
        {
            // The blocking-door value went EMPTY — the corridor is clear: the
            // door opened (e.g. a dungeon event freed the prisoner who unlocked
            // the courtyard door) or there was never a real blocker. Do NOT
            // auto-pause; drop any stall and stand down so Advance walks the now-
            // open doorway next tick. Without this, the one-tick race between the
            // door opening and the cached blocking-door value clearing paused the
            // run "the way is blocked by a door" the instant the event succeeded.
            ClearStall(context);
            return true;
        }
        // A non-empty guid we couldn't resolve (the GO's grid isn't resident) —
        // genuinely unknown; fall back to the in-place stall so the reason is
        // still reported.
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] door-blocked: door guid {} unresolved -> parking in place",
                 bot->GetName(), doorGuid.ToString());
        return parkAndStall();
    }

    // GetExactDist is to the door's GO origin (hinge/jamb) — kept only for log
    // lines. It is NOT used to decide when to park: the origin can sit past the
    // doorway gap, so "within Nyd of the origin" made the walk-in glide carry the
    // tank THROUGH the gap to reach it. Parking is decided below on distance
    // travelled ALONG the path to the door (DistAlongPathToClosedDoor).
    float const distToDoor = bot->GetExactDist(door);

    // --- Walk the WHOLE corridor up to the door --------------------------
    // The door is detected as much as 80yd ahead along the route, so we must
    // close that whole gap before parking. The cached long-path to the next
    // boss is itself truncated by this very door (the closed GO blocks the
    // navmesh), so its polyline already terminates right at the door — glide
    // that polyline to its end and the tank lands AT the door. The previous
    // code did MoveTo(door): that overload bee-lines to the door and clamps
    // each step to spellDistance, projecting an intermediate point straight at
    // the door through any wall on a winding corridor, so the tank stopped at
    // the last reachable spot far short of it. Reuse the same escort-spline
    // follower Advance drives (shared follower state, same long-path value).
    DcApproachState& appr =
        context->GetValue<DcApproachState&>("dungeon clear approach state")->Get();
    if (next.has_value())
    {
        // Match Advance: route toward the boss's EFFECTIVE position (live
        // creature coords when loaded, else the static anchor) so this shares
        // the same cached long-path Advance builds. Targeting the static anchor
        // here while Advance targets the live boss would flip the shared cache's
        // built-toward position every tick and thrash rebuilds.
        Creature* const liveBoss = DcTargeting::GetLiveBoss(bot, context, next->entry);
        DungeonBossInfo effectiveTarget = *next;
        if (liveBoss)
        {
            effectiveTarget.x = liveBoss->GetPositionX();
            effectiveTarget.y = liveBoss->GetPositionY();
            effectiveTarget.z = liveBoss->GetPositionZ();
        }
        EnsureLongPath(bot, context, appr, effectiveTarget);
    }

    ChunkedPathfinder::Result const& path =
        AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
    if (!path.reachable || path.segments.empty())
    {
        // No corridor to follow (boss-side route gone). Hold the door line
        // from wherever we are rather than thrash.
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] door-blocked: no long-path corridor ({:.1f}yd from door) "
                  "-> park in place",
                  bot->GetName(), distToDoor);
        return parkAndStall();
    }

    // Park on the NEAR side: stop once the route is within DC_DOOR_STOP_DISTANCE
    // (travel distance) of reaching the doorway. Measured along the path, so it
    // halts before the gap even when the door's GO origin sits past it and the
    // navmesh runs the corridor straight through the shut door. Without this the
    // walk-in glided through the gap to get "close to the origin", then the pack
    // beyond aggroed the now-through tank.
    float const distAlongToDoor =
        DcEngageGeometry::DistAlongPathToClosedDoor(
            bot, path, door->GetPositionX(), door->GetPositionY(),
            door->GetPositionZ(), /*lookAhead*/ 100.0f);
    if (distAlongToDoor <= DC_DOOR_STOP_DISTANCE)
    {
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] door-blocked: at door ({:.1f}yd along path) -> parking, "
                  "waiting for it to open",
                  bot->GetName(), distAlongToDoor);
        return parkAndStall();
    }

    DungeonFollowerState& follower =
        context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get();

    // Off-path recovery (knockback / follower bump while walking in), mirrors
    // Advance: re-anchor onto the existing polyline, or rebuild + hold.
    if (DungeonPathFollower::IsOffPath(bot, path, follower) &&
        follower.offPathTicks >= DungeonPathFollower::OFF_PATH_TICK_LIMIT)
    {
        if (!DungeonPathFollower::Resnap(bot, path, follower))
        {
            DcMovement::ResolveEscortConflict(bot);
            appr.longPathExpiresMs = 0;
            follower = DungeonFollowerState{};
            return parkAndStall();
        }
    }

    DungeonPathFollower::Hop hop = DungeonPathFollower::NextHop(bot, path, follower);
    if (hop.isDone)
    {
        // Reached the end of the corridor = as close to the door as the
        // navmesh allows (the door's collision truncates the route here). This
        // is the real "at the door"; park and wait.
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] door-blocked: corridor end reached ({:.1f}yd from door) -> parking",
                  bot->GetName(), distToDoor);
        return parkAndStall();
    }

    // Leave a healthy in-flight spline alone, but ONLY while it is genuinely
    // gliding (same splineRunning-only guard as Advance — gating on
    // IsWaitingForLastMove froze the walk-in for the remainder of the
    // window-sized delay whenever the spline finalized early).
    MotionMaster* mm = bot->GetMotionMaster();
    bool const splineRunning =
        mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE && bot->isMoving();
    if (splineRunning)
        return true;
    if (!IsMovingAllowed())
        return parkAndStall();

    uint32 const mapId = bot->GetMapId();

    // A jump leg en route to the door (drop-down corridor) — arc it.
    if (hop.isJump)
    {
        JumpTo(mapId, hop.point.x, hop.point.y, hop.point.z, MovementPriority::MOVEMENT_NORMAL);
        ClearStall(context);
        return true;
    }

    // Re-entry leg must be a generated path (same rationale as Advance): if a
    // bump/knockback left the bot off the corridor, the escort spline's opening
    // straight leg back to the route clips wall corners. Rejoin via PathGenerator
    // (MoveTo) while off the line; the glide resumes once back on it.
    float const deviation = DungeonPathFollower::RouteDeviation(bot, path, follower);
    if (deviation > DungeonPathFollower::OFF_PATH_THRESHOLD)
    {
        DcMoveTo(mapId, hop.point.x, hop.point.y, hop.point.z,
                 /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                 /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] door walk-in off-line {:.1f}yd -> rejoining route via "
                  "generated path (seg {} pt {})",
                  bot->GetName(), deviation, follower.segmentIdx, follower.pointIdx);
        ClearStall(context);
        return true;
    }

    // Continuous escort spline along the upcoming polyline run, identical to
    // Advance's glide — linear spline, wall-safe, no per-point stops. SplinePath
    // owns the stand-up / cast-interrupt / MoveSplinePath ritual + LastMovement
    // record and refuses a <2-point window.
    std::vector<G3D::Vector3> const window =
        DungeonPathFollower::BuildSplineWindow(bot, path, follower);
    Movement::PointsArray points(window.begin(), window.end());
    if (DcMovement::SplinePath(botAI, points))
    {
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] door walk-in spline: {} pts ({:.1f}yd to door, seg {} pt {})",
                  bot->GetName(), points.size(), distToDoor,
                  follower.segmentIdx, follower.pointIdx);
        ClearStall(context);
        return true;
    }

    // Window < 2 points (lone anchor tail): short single-hop fallback.
    DcMoveTo(mapId, hop.point.x, hop.point.y, hop.point.z,
             /*idle*/ false, /*react*/ false, /*normal_only*/ false,
             /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);
    ClearStall(context);
    return true;
}

