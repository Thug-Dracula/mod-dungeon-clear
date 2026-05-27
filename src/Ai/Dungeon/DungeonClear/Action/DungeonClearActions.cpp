/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearActions.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "Creature.h"
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
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Playerbots.h"

namespace
{
    // MoveTo-returned-false counter. Raised from 2 to 8 because dedup
    // (IsDuplicateMove returning true while the bot is making real progress
    // on the prior move) was tripping the original threshold during normal
    // operation. The position-based detector below is the authoritative
    // "actually stuck" signal; this counter survives only as a backup for
    // the case where the bot is stationary AND MoveTo keeps refusing.
    constexpr uint32 DC_STUCK_LIMIT = 8;

    // Position-based stuck detection. If the bot is supposed to be moving
    // but its world position barely shifts for DC_STUCK_TICK_LIMIT
    // consecutive Advance ticks, treat as stuck. Catches the "ran into
    // wall / shortcut path / stuck in mmap seam" case that the MoveTo-
    // returned-false counter misses.
    constexpr float DC_STUCK_DISPLACEMENT = 1.5f;
    constexpr uint32 DC_STUCK_TICK_LIMIT = 5;

    // Must match DungeonClearTriggers.cpp.
    constexpr float DC_ENGAGE_RANGE = 22.0f;
    constexpr float DC_ENGAGE_CONE_RANGE = 35.0f;
    constexpr float DC_ENGAGE_CONE_HALF_ANGLE = static_cast<float>(M_PI) / 3.0f;
    constexpr float DC_REST_MIN_HP_PCT = 90.0f;
    constexpr float DC_REST_MIN_MP_PCT = 75.0f;
    constexpr float DC_PARTY_MAX_SPREAD = 40.0f;
    constexpr bool  DC_USE_CORRIDOR_SCAN = true;
    constexpr float DC_CORRIDOR_LOOKAHEAD = 35.0f;
    // Half-width of the path "blocking trash" band. Widened from 8 to 18 so it
    // roughly matches level-80 elite aggro radius: a pack sitting a few yards
    // off the route line still aggros as the tank passes, so it must count as
    // blocking trash. With the old 8yd band such a pack was never a candidate,
    // so the selector picked an on-line mob farther ahead and the tank ran
    // straight through the side pack to reach it. Must match DungeonClearTriggers.cpp.
    constexpr float DC_CORRIDOR_WIDTH = 18.0f;

    // Any hostile already inside this radius of the bot will pull as the tank
    // moves, regardless of whether it sits on the corridor line. The engage
    // selector hits the closest such mob first so the tank never runs past an
    // adjacent pack to reach a farther on-corridor target.
    constexpr float DC_PROXIMITY_ENGAGE_RANGE = 20.0f;

    // Distance from the bot to its next polyline hop above which the follower
    // cursor is treated as stale and force-re-anchored (Resnap). During clean
    // gliding the next hop is only ~one polyline step ahead (~4-8yd), so a
    // larger gap means the tank was displaced off its cursor — almost always
    // by a trash chase (EngageDirect walk + combat MoveChase). Unlike the
    // perpendicular IsOffPath check, this also catches ALONG-track
    // displacement (chased forward past the cursor), which would otherwise
    // make NextHop target a point behind the tank and walk it backward.
    constexpr float DC_REANCHOR_DISTANCE = 12.0f;

    // COMBAT-priority movement can't be interrupted by the bot's own combat
    // reflexes (see DungeonClearAdvanceAction). Only use it for the final
    // approach within this distance of attack range; past that, approach at
    // NORMAL so the tank stops to fight packs it aggros en route instead of
    // plowing through them to a locked target.
    constexpr float DC_COMBAT_APPROACH_RANGE = 10.0f;

    // How far in front of a blocking door the tank parks while waiting for it
    // to be opened. Close enough to read as "standing at the door", far enough
    // not to clip into the GO model. The blocking-door detector uses a 5yd
    // corridor half-width, so this stays inside that band.
    constexpr float DC_DOOR_STOP_DISTANCE = 4.0f;

    // The creature store (Map::GetCreatureBySpawnIdStore) only contains
    // creatures in LOADED grids; grids stream in within ~MAX_VISIBILITY_DISTANCE
    // (250yd) of a moving player. Beyond this distance, a boss simply not being
    // in the store means its grid hasn't loaded yet — NOT that it isn't spawned.
    // So Advance keeps walking toward the boss's static spawn coords to load the
    // grid en route instead of stalling. Kept comfortably under 250yd so the
    // grid is certainly resident by the time we'd declare the boss truly missing.
    constexpr float DC_BOSS_GRID_LOADED_RANGE = 150.0f;

    // Once the boss creature is loaded and visible within this range, pursue its
    // LIVE position directly (per-tick re-path) instead of walking the long-path
    // to its static DB spawn anchor. A wandering/patrolling boss is rarely at
    // its spawn point; without this the tank walked to the anchor, parked
    // ~DC_ENGAGE_RANGE short of it, and idled until the boss patrolled back into
    // range on its own (it aggroed the tank rather than the reverse). LOS-gated
    // so a boss around a corner still routes through the wall-screened
    // long-path. Generous because it is only a final-approach shortcut for an
    // already-visible boss.
    constexpr float DC_DIRECT_PURSUIT_RANGE = 80.0f;

    // The long-path can complete (cursor reaches the polyline end) while the bot
    // is still outside DC_ENGAGE_RANGE of the boss: the navmesh route dead-ends
    // short (boss on a ledge / across a gap, wall-screened route that can't close
    // the last yards). NextHop reports done, Advance rebuilds an identical
    // 0-point path, and because the bot isn't moving the position-based stuck
    // counter never fires — a silent forever-loop (observed: WC Lady Anacondra
    // spun here ~3 min until the log was cut). For this many consecutive
    // done-but-not-engaged ticks Advance tries a straight final-approach MoveTo
    // (PathGenerator may close a few yards Detour's chunk builder gave up on);
    // past it the boss is declared unreachable and we stall for `dc skip`.
    constexpr uint32 DC_DONE_NOT_ENGAGED_LIMIT = 15;

    // Long-path cache TTL. Boss positions are static, so a longer TTL is
    // safe; rebuild costs are bounded (~8 PathGenerator calls × sub-ms each).
    // Keeping the TTL short keeps stale paths from outlasting edge cases
    // like portal traversal or stuck-teleport recovery.
    constexpr uint32 DC_LONG_PATH_TTL_MS = 15 * 1000;

    // Commit-timeout for the loot yield, shared by the tank's advance yield and
    // the follower's follow-tank yield. After a pull the tank holds until the
    // WHOLE party has finished looting (see the advance loot-yield block), and
    // each follower steps off follow to walk in to its own corpses — both for
    // at most this long. Past it the tank force-advances toward the next boss
    // and followers resume following, instead of the party parking forever on a
    // corpse it can't finish (group-loot rolls pending, bags full, un-pickable).
    // 15s comfortably covers several members each walking in from lootDistance
    // (15yd / ~7yd/s ≈ 2s) and grabbing multiple items, while still bounding a
    // wedge. This is the "reasonable timeout, then move on" window.
    constexpr uint32 DC_LOOT_YIELD_TIMEOUT_MS = 15 * 1000;

    // Recovery moves run when the bot is wedged off the navmesh or has
    // failed to make progress for DC_STUCK_TICK_LIMIT consecutive ticks.
    // Single-player server only — the teleport blink is visible to other
    // players. Flip to false to disable both shims and keep the legacy
    // "stall and wait for `dc skip`" behavior.
    constexpr bool DC_ALLOW_RECOVERY_MOVES = true;
    // 5yd offsets for the FARFROMPOLY-START recovery; small enough that
    // the bot doesn't significantly mis-position, large enough to clear
    // the off-mesh poly the bot may have wedged on.
    constexpr float DC_RECOVERY_OFFSET = 5.0f;

    // Optional pre-attack ranged pull. Fire one class-appropriate cast at
    // the engage target before the existing auto-attack engagement runs.
    // If the cast lands, the tank gets the threat lead that auto-attack
    // alone never provides. If anything fails (cooldown, silenced, target
    // out of range, spell not known) we just fall through to auto-attack
    // — the existing reliable path is never replaced. Set to false to
    // restore previous behavior exactly.
    constexpr bool  DC_TRY_PULL_SPELL = true;

    // Short label for the active movement generator, for advance telemetry.
    // Names the types the dungeon-clear follower drives (ESCORT/POINT) or
    // fights against (CHASE/FOLLOW = combat/leader movement overriding the
    // escort spline); the caller also prints the raw enum value alongside.
    char const* MoveGenTypeName(MovementGeneratorType t)
    {
        switch (t)
        {
            case IDLE_MOTION_TYPE:   return "IDLE";
            case CHASE_MOTION_TYPE:  return "CHASE";
            case POINT_MOTION_TYPE:  return "POINT";
            case FOLLOW_MOTION_TYPE: return "FOLLOW";
            case ESCORT_MOTION_TYPE: return "ESCORT";
            case HOME_MOTION_TYPE:   return "HOME";
            case NULL_MOTION_TYPE:   return "NULL";
            default:                 return "OTHER";
        }
    }

    struct PullSpellPick
    {
        std::string name;
        float minRange;
        float maxRange;
    };

    // Per-class single best opener. Names match the strings the engine
    // already registers via the per-class AiObjectContext (e.g. "heroic
    // throw", "avenger's shield"). No fallback chain — `botAI->CastSpell`
    // returns false cleanly if the spell isn't known/usable, and we'll
    // just auto-attack instead.
    std::optional<PullSpellPick> PickPullSpell(Player* bot)
    {
        if (!bot)
            return std::nullopt;
        switch (bot->getClass())
        {
            case CLASS_WARRIOR:      return PullSpellPick{"heroic throw",       8.0f, 30.0f};
            case CLASS_PALADIN:      return PullSpellPick{"avenger's shield",   8.0f, 30.0f};
            case CLASS_DEATH_KNIGHT: return PullSpellPick{"death grip",         8.0f, 30.0f};
            // Bear-tank druids use Faerie Fire (Feral) — 30yd, no form
            // switch, applies armor debuff + threat. Caster druids would
            // fail this cast (spell not known) and just auto-attack
            // instead, which is fine — DC is tank-only anyway.
            case CLASS_DRUID:        return PullSpellPick{"faerie fire (feral)", 8.0f, 30.0f};
            default:                 return std::nullopt;
        }
    }

    void DisableDungeonClear(PlayerbotAI* botAI, std::string const& reason)
    {
        AiObjectContext* ctx = botAI->GetAiObjectContext();
        ctx->GetValue<bool>("dungeon clear enabled")->Set(false);
        ctx->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear stuck ticks")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear last target entry")->Set(0u);
        ctx->GetValue<Position&>("dungeon clear last position")->Get() = Position();
        ctx->GetValue<ObjectGuid>("dungeon clear last pull target")->Set(ObjectGuid::Empty);
        ctx->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
        ctx->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
        ctx->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
        ctx->GetValue<uint32>("dungeon clear long path target")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear stride rebuild attempts")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear loot yield start")->Set(0u);
        ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
        ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};
        DungeonClearUtil::SendAddonMessage(botAI, "CHAT\t" + reason);
        botAI->DoSpecificAction("dc status", Event(), true);
    }

    // Mode stays enabled. Sets a stall reason that the fallback trigger uses
    // to fire, and that `dc status` can report. Announces the reason in party
    // chat the first time it changes — repeats are suppressed until either
    // the reason text changes or `dc skip`/`dc on`/`dc off` clears it.
    void StallDungeonClear(PlayerbotAI* botAI, std::string const& reason)
    {
        AiObjectContext* ctx = botAI->GetAiObjectContext();
        ctx->GetValue<std::string&>("dungeon clear stall reason")->Get() = reason;

        std::string& lastSaid = ctx->GetValue<std::string&>("dungeon clear last said reason")->Get();
        if (lastSaid != reason)
        {
            lastSaid = reason;
            DungeonClearUtil::SendAddonMessage(botAI, "CHAT\t" + reason);
            botAI->DoSpecificAction("dc status", Event(), true);
        }
    }

    void ClearStall(AiObjectContext* ctx)
    {
        ctx->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
        ctx->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    }

    // Party-readiness gate for resuming the advance (HP/MP/spread). Loot is
    // handled separately in Execute so it can enforce a commit-timeout; see
    // the loot-yield block there.
    bool IsBetweenPullsReady(Player* bot)
    {
        return DungeonClearUtil::IsPartyReady(bot, DC_REST_MIN_HP_PCT, DC_REST_MIN_MP_PCT, DC_PARTY_MAX_SPREAD);
    }

    // Rebuild the cached long-path when the boss entry changes or the
    // TTL expires. Idempotent — safe to call every Advance tick. Any
    // rebuild also resets the follower state so we don't index off the
    // end of a fresh, shorter polyline.
    void EnsureLongPath(Player* bot, AiObjectContext* ctx, DungeonBossInfo const& target)
    {
        uint32& cachedEntry = ctx->GetValue<uint32>("dungeon clear long path target")->RefGet();
        uint32& expiresAt = ctx->GetValue<uint32>("dungeon clear long path expires")->RefGet();
        uint32 const now = getMSTime();
        bool const targetChanged = cachedEntry != target.entry;
        bool const expired = expiresAt == 0 || now >= expiresAt;
        if (!targetChanged && !expired)
            return;

        ChunkedPathfinder::Result& path =
            ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Get();
        path = ChunkedPathfinder::Build(bot, target.mapId, target.entry, target.x, target.y, target.z);
        cachedEntry = target.entry;
        expiresAt = now + DC_LONG_PATH_TTL_MS;

        size_t firstSegPts = path.segments.empty() ? 0 : path.segments.front().polyline.size();
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] path rebuilt -> {} ({}): segs={} firstSegPts={} complete={}{}",
                 bot->GetName(), target.name, targetChanged ? "boss changed" : "TTL/forced",
                 path.segments.size(), firstSegPts, path.complete,
                 path.reachable ? "" : (" UNREACHABLE: " + path.failureReason));

        ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
        ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};
    }

    // Try a small offset move when the bot wedges on geometry off the
    // navmesh (PATHFIND_FARFROMPOLY_START). Walks four cardinal offsets;
    // picks the first one whose PathGenerator probe returns a usable
    // path (NORMAL or INCOMPLETE — even partial is enough for recovery).
    //
    // Returns true if a recovery move was issued. False means none of
    // the offsets looked recoverable; caller stalls normally.
    bool TryFarFromPolyRecovery(Player* bot)
    {
        if (!bot)
            return false;
        float const x = bot->GetPositionX();
        float const y = bot->GetPositionY();
        float const z = bot->GetPositionZ();
        struct Offset { float dx, dy; };
        Offset const offsets[] = {
            {DC_RECOVERY_OFFSET, 0.0f},
            {-DC_RECOVERY_OFFSET, 0.0f},
            {0.0f, DC_RECOVERY_OFFSET},
            {0.0f, -DC_RECOVERY_OFFSET},
        };
        for (Offset const& o : offsets)
        {
            float const nx = x + o.dx;
            float const ny = y + o.dy;
            float nz = z;
            bot->UpdateAllowedPositionZ(nx, ny, nz);
            PathGenerator gen(bot);
            gen.CalculatePath(nx, ny, nz, /*forceDest*/ false);
            PathType const t = gen.GetPathType();
            // Accept anything that produced a real point path — we just
            // need to budge onto a polygon. The chunked rebuild on the
            // next tick handles the actual route from the new position.
            if (t & PATHFIND_NOPATH)
                continue;
            if (t & PATHFIND_FARFROMPOLY_START)
                continue;  // didn't actually help — still off the mesh

            MotionMaster* mm = bot->GetMotionMaster();
            if (mm)
                mm->MovePoint(0, nx, ny, nz, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, /*generatePath*/ true, false);
            return true;
        }
        return false;
    }

    // Forward-recovery: try a cheap polyline Resnap first — the bot is
    // often only a few yards off the planned corridor (sticky-trash
    // detour, follower bump, micro-knockback) and reusing the existing
    // path is faster and visually less disruptive than rebuilding. If
    // Resnap fails, invalidate the cache and reset the follower so the
    // next Advance tick rebuilds the route from the bot's current poly.
    //
    // The v1 design used a back-teleport (NearTeleportTo to the previous
    // segment) for this case, but that hurt as often as it helped — the
    // bot would teleport backward, re-run the same builder from the same
    // point, and get the same wrong route. Returning false here yields the
    // tick without issuing movement so Advance can re-enter cleanly.
    //
    // Returns true when Resnap kept us on the existing path; false when
    // a full rebuild is needed (in which case the cache/state are reset).
    bool TriggerStrideRebuild(Player* bot, AiObjectContext* ctx)
    {
        ChunkedPathfinder::Result const& path =
            ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Get();
        DungeonFollowerState& follower =
            ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get();
        if (path.reachable && !path.segments.empty() &&
            DungeonPathFollower::Resnap(bot, path, follower))
            return true;

        ctx->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
        ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
        follower = DungeonFollowerState{};
        return false;
    }

    // Halt an in-flight continuous-spline glide (see Advance's spline path)
    // so that a forced rebuild/reset on the next tick starts from a
    // standstill. Without this, a now-stale EscortMovementGenerator spline
    // would keep driving the bot down the OLD route while the rebuilt path
    // is ignored (Advance's splineRunning guard would treat the stale glide
    // as healthy). Only touches our own escort glide; no-op otherwise.
    void StopActiveSplineGlide(Player* bot)
    {
        if (!bot)
            return;
        MotionMaster* mm = bot->GetMotionMaster();
        if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE)
            bot->StopMoving();
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
        // Optional: try a single class pull-spell opener before walking in.
        // Fire-and-forget — if CastSpell returns false (out of range, on
        // cooldown, silenced, not known), we just fall through to the walk.
        // Suppress repeats while we're closing on the same target so we
        // don't spam casts each tick.
        if (DC_TRY_PULL_SPELL)
        {
            ObjectGuid const lastPullTarget =
                AI_VALUE(ObjectGuid, "dungeon clear last pull target");
            if (lastPullTarget != target->GetGUID())
            {
                if (auto pick = PickPullSpell(bot))
                {
                    if (distance >= pick->minRange && distance <= pick->maxRange &&
                        bot->IsWithinLOSInMap(target))
                    {
                        bot->SetSelection(target->GetGUID());
                        if (botAI->CastSpell(pick->name, target))
                        {
                            context->GetValue<ObjectGuid>("dungeon clear last pull target")
                                ->Set(target->GetGUID());
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
        return MoveTo(target->GetMapId(),
                      target->GetPositionX(),
                      target->GetPositionY(),
                      target->GetPositionZ(),
                      /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                      /*exact_waypoint*/ false, prio);
    }

    if (bot->isMoving())
        bot->StopMoving();

    bot->SetSelection(target->GetGUID());
    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    context->GetValue<Unit*>("current target")->Set(target);
    bot->Attack(target, melee);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
    return true;
}

bool DungeonClearAdvanceAction::Execute(Event /*event*/)
{
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
    {
        // Mode stays enabled so `dc skip` is still reachable, but there is
        // nothing to skip from at this point — the next-boss value is empty
        // because every remaining boss is dead or already skipped.
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] advance: no next boss (all dead/skipped) -> stalling",
                 bot->GetName());
        StallDungeonClear(botAI,
            "Can't find a next boss: all remaining bosses are marked dead or skipped — try 'dc bosses' to inspect.");
        return false;
    }

    // Effective boss position: a wandering/patrolling boss is rarely at its
    // static DB spawn coords, so prefer its LIVE creature position whenever it
    // is loaded on the map. Engage-range gating, the at-boss handoff, and the
    // final-approach pursuit below all key off this, so the tank chases where
    // the boss actually is instead of parking at the spawn anchor. Falls back to
    // the static coords when the creature isn't loaded (far grid not streamed in
    // yet — see DC_BOSS_GRID_LOADED_RANGE).
    Creature* const liveBoss = DungeonClearUtil::FindLiveCreatureOnMap(bot, next->entry);
    float const bossX = liveBoss ? liveBoss->GetPositionX() : next->x;
    float const bossY = liveBoss ? liveBoss->GetPositionY() : next->y;
    float const bossZ = liveBoss ? liveBoss->GetPositionZ() : next->z;
    float const engageDist = bot->GetDistance(bossX, bossY, bossZ);

    // Dead-end escalation counter (see DC_DONE_NOT_ENGAGED_LIMIT). Cleared the
    // moment we're back inside engage range so it only ever counts a continuous
    // run of "path done but boss still out of reach" ticks for one approach.
    uint32& doneNotEngagedTicks =
        context->GetValue<uint32>("dungeon clear done-not-engaged ticks")->RefGet();
    if (engageDist < DC_ENGAGE_RANGE)
        doneNotEngagedTicks = 0;

    // Already in engage range of the (live) boss AND no anchored intermediate
    // hops remain unresolved → stop walking and let the at-boss trigger fire the
    // pull. With anchored routes the bot may be geometrically near the boss but
    // still on the wrong side of a wall; we keep walking in that case until
    // anchors are cleared.
    if (engageDist < DC_ENGAGE_RANGE)
    {
        ChunkedPathfinder::Result const& currentPath =
            AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
        DungeonFollowerState const& followerNow =
            AI_VALUE(DungeonFollowerState&, "dungeon clear follower state");
        bool anchoredHopsPending = false;
        if (currentPath.reachable && !currentPath.segments.empty())
        {
            // Only inspect segments still ahead of the follower's cursor.
            // Anchored segments already walked past don't need to gate
            // the engage handoff.
            for (size_t i = followerNow.segmentIdx; i + 1 < currentPath.segments.size(); ++i)
            {
                PathSegment const& seg = currentPath.segments[i];
                if (seg.anchored && bot->GetDistance(seg.ex, seg.ey, seg.ez) > seg.arriveRadius)
                {
                    anchoredHopsPending = true;
                    break;
                }
            }
        }
        if (!anchoredHopsPending)
        {
            // Surface WHY we're holding: the at-boss trigger only pulls once the
            // party is ready and no loot is pending. When it doesn't fire, this
            // is the line that explains the otherwise-silent idle at the boss.
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] within engage range of {} ({:.0f}yd, live={}) -> holding "
                      "for at-boss [partyReady={} availLoot={} canLoot={}]",
                      bot->GetName(), next->name, engageDist, liveBoss ? 1 : 0,
                      IsBetweenPullsReady(bot) ? 1 : 0,
                      AI_VALUE(bool, "has available loot") ? 1 : 0,
                      AI_VALUE(bool, "can loot") ? 1 : 0);
            if (bot->isMoving())
                bot->StopMoving();
            ClearStall(context);
            return false;
        }
    }

    // Loot yield (with commit-timeout). Step aside through the WHOLE loot
    // lifecycle so the loot system can pick up a nearby corpse: "has available
    // loot" is true only while a corpse is ~3-15yd away and flips FALSE at ~3yd
    // (when "can loot" flips TRUE). Advance (engine relevance 15) outranks the
    // loot actions (open loot is 8), so yielding on only one flag let advance
    // win the tick at the 3yd boundary and fire a boss-bound spline before
    // open-loot ran — the corpse<->boss oscillation. Yielding while EITHER flag
    // is set keeps advance out of the way until the loot is actually picked up.
    //
    // We also hold while ANY follower still has a corpse to pick up, so the
    // tank doesn't push to the next pull the instant its own loot is done and
    // leave the party scrambling to catch up. IsAnyPartyMemberLooting reads
    // each follower's own loot flags cross-context (same pattern as the party-
    // tank lookup); the shared commit-timeout below bounds the total wait.
    //
    // The timeout stops us waiting forever on loot the party can't finish
    // (group-loot rolls pending, bags full): after DC_LOOT_YIELD_TIMEOUT_MS we
    // force-advance; when no one is looting any more the flags clear and the
    // timer resets (so the next pull gets a fresh full window).
    uint32& lootYieldStart =
        context->GetValue<uint32>("dungeon clear loot yield start")->RefGet();
    bool const lootYield =
        AI_VALUE(bool, "has available loot") || AI_VALUE(bool, "can loot") ||
        DungeonClearUtil::IsAnyPartyMemberLooting(bot);
    if (lootYield)
    {
        uint32 const now = getMSTime();
        if (lootYieldStart == 0)
            lootYieldStart = now;

        if (now - lootYieldStart >= DC_LOOT_YIELD_TIMEOUT_MS)
        {
            // Waited long enough — give up on this corpse and advance past it.
            // Don't reset lootYieldStart here: keep it expired so we keep
            // advancing each tick until we clear lootDistance (which resets it).
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] loot-yield timed out after {}ms -> force-advancing past corpse",
                     bot->GetName(), now - lootYieldStart);
        }
        else
        {
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] advance yielding: loot in progress ({}ms)",
                      bot->GetName(), now - lootYieldStart);
            if (bot->isMoving())
                bot->StopMoving();
            return false;
        }
    }
    else
    {
        lootYieldStart = 0;  // not looting -> reset the commit timer
    }

    // Between-pulls rest: yield so food/drink can run and stragglers catch up.
    // The multiplier suppresses wander actions during the wait.
    if (!IsBetweenPullsReady(bot))
    {
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] advance yielding: party not ready / resting", bot->GetName());
        if (bot->isMoving())
            bot->StopMoving();
        return false;
    }

    // If this boss has no live spawn at all (and not even a corpse), stall
    // so the player can `dc skip` instead of being forced to re-enable the
    // mode. Bosses that legitimately despawn after kill are handled by the
    // InstanceScript::GetBossState probe in NextDungeonBossValue — they
    // never reach here.
    if (!DungeonClearUtil::IsCreaturePresentOnMap(bot, next->entry))
    {
        // "Not present" only means "not spawned" once we're close enough that
        // the boss's grid is certainly loaded. While we're still far, the grid
        // simply hasn't streamed in yet (see DC_BOSS_GRID_LOADED_RANGE). Hard-
        // stalling here froze the tank at the edge of a large room and it never
        // walked in to load the grid -> deadlock; and because this returns
        // before EnsureLongPath, with zero DC-channel output. Fall through and
        // let Advance path toward the boss's static spawn coords instead.
        float const distToBoss = bot->GetDistance(next->x, next->y, next->z);
        if (distToBoss <= DC_BOSS_GRID_LOADED_RANGE)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] {} not in creature store at {:.0f}yd (<={:.0f}, grid "
                     "loaded) -> stalling: genuinely not spawned",
                     bot->GetName(), next->name, distToBoss, DC_BOSS_GRID_LOADED_RANGE);
            StallDungeonClear(botAI,
                "Can't reach " + next->name + ": not spawned on this map. Use 'dc skip' to move to the next boss.");
            return false;
        }
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] {} not in creature store but {:.0f}yd away (>{:.0f}) "
                  "-> advancing to stream its grid in",
                  bot->GetName(), next->name, distToBoss, DC_BOSS_GRID_LOADED_RANGE);
        // fall through to the normal advance below
    }

    // Bookkeeping: reset position-based stuck counters when the boss
    // changes, so an in-flight stuck count from the previous pull doesn't
    // bleed into the new approach.
    uint32 const lastEntry = AI_VALUE(uint32, "dungeon clear last target entry");
    uint32& stuck = context->GetValue<uint32>("dungeon clear stuck count")->RefGet();
    Position& lastPos = context->GetValue<Position&>("dungeon clear last position")->Get();
    uint32& posStuck = context->GetValue<uint32>("dungeon clear stuck ticks")->RefGet();
    uint32& rebuildAttempts =
        context->GetValue<uint32>("dungeon clear stride rebuild attempts")->RefGet();
    if (lastEntry != next->entry)
    {
        context->GetValue<uint32>("dungeon clear last target entry")->Set(next->entry);
        stuck = 0;
        posStuck = 0;
        rebuildAttempts = 0;
        doneNotEngagedTicks = 0;
        lastPos = Position();  // reset to (0,0,0) sentinel on boss change
        // Sticky trash target was picked for the previous boss's corridor;
        // it doesn't necessarily lie in the new boss's corridor and can be
        // far behind us. Re-pick from scratch on next engage-trash tick.
        context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    }

    // Position-based stuck check. Sample current world position; if the
    // bot is supposedly moving but barely shifted since the previous tick,
    // increment posStuck. The (0,0,0) lastPos is the not-yet-sampled
    // sentinel — no real dungeon map has a (0,0,0) walkable point.
    Position const cur(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    bool const lastPosValid =
        lastPos.m_positionX != 0.0f || lastPos.m_positionY != 0.0f || lastPos.m_positionZ != 0.0f;
    if (lastPosValid && bot->isMoving() && cur.GetExactDist(lastPos) < DC_STUCK_DISPLACEMENT)
        ++posStuck;
    else
    {
        posStuck = 0;
        // Real forward progress clears any prior consecutive-rebuild count.
        // (Lastposvalid + movement above DC_STUCK_DISPLACEMENT is the
        // strongest signal we have that the route just resumed working.)
        if (lastPosValid && bot->isMoving())
            rebuildAttempts = 0;
    }

    // Per-tick advance telemetry — the three signals the spline-issue lines
    // can't show on their own: did the bot physically move since the last
    // Advance tick (posDelta), which generator is in control right now, and
    // is combat movement involved. Read against the timestamps of the
    // "spline issued" / "re-anchor" / "off-path" lines, this disambiguates
    // the pacing wedge: a posDelta ~0 right after a spline issuance means the
    // spline was issued but never travelled; a CHASE/FOLLOW gen here means
    // combat/leader movement is overriding the escort spline; an ESCORT gen
    // with posDelta ~0 means the spline launched but wedged against geometry.
    {
        MotionMaster* const tmm = bot->GetMotionMaster();
        MovementGeneratorType const gen =
            tmm ? tmm->GetCurrentMovementGeneratorType() : NULL_MOTION_TYPE;
        float const posDelta = lastPosValid ? cur.GetExactDist(lastPos) : -1.0f;
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] advance tick: posDelta={:.2f}yd moving={} gen={}({}) combat={}",
                  bot->GetName(), posDelta, bot->isMoving() ? 1 : 0,
                  MoveGenTypeName(gen), static_cast<uint32>(gen),
                  bot->IsInCombat() ? 1 : 0);
    }

    lastPos = cur;

    if (posStuck >= DC_STUCK_TICK_LIMIT)
    {
        posStuck = 0;

        // The bot was moving but not progressing — a continuous-spline glide
        // wedged against geometry. Halt it so the recovery below re-issues
        // movement from a standstill instead of fighting the stuck spline.
        StopActiveSplineGlide(bot);

        // First-line recovery: try a Resnap onto the existing polyline
        // (cheap; handles the "knocked sideways but path is still good"
        // case). On failure, invalidate the long-path cache and reset
        // the follower so the next tick rebuilds from the bot's current
        // position. Strides are short enough that a rebuild from here
        // usually picks a different sequence of stride endpoints and
        // routes around whatever was wedging us.
        bool const resnapped = TriggerStrideRebuild(bot, context);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] posStuck ({} ticks <{}yd) -> {} (rebuildAttempts={})",
                 bot->GetName(), DC_STUCK_TICK_LIMIT, DC_STUCK_DISPLACEMENT,
                 resnapped ? "resnapped onto existing route" : "forcing rebuild",
                 rebuildAttempts + (resnapped ? 0u : 1u));
        if (resnapped)
        {
            // Resnap fixed us without burning a rebuild — leave the
            // rebuild-attempt counter alone so the navmesh-nudge
            // escalation only triggers on true geometric wedges, not
            // on transient drifts.
            return false;
        }
        ++rebuildAttempts;

        // After three consecutive rebuilds without forward progress, try a
        // small navmesh-nudge: the bot may be on a poly the chunked builder
        // can't reach (off-corridor, layered geometry seam). The 5yd offset
        // probes are deliberately tiny so we don't significantly mis-position.
        if (rebuildAttempts >= 3)
        {
            rebuildAttempts = 0;
            if (DC_ALLOW_RECOVERY_MOVES && TryFarFromPolyRecovery(bot))
            {
                DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tRepathing around " + next->name + " \xe2\x80\x94 nudging onto the navmesh.");
                return true;
            }
            StallDungeonClear(botAI,
                "Stuck near " + next->name + " — not making forward progress. "
                "I'll try to clear nearby mobs; use 'dc skip' if it persists.");
            return false;
        }
        return false;
    }

    // Final-approach pursuit of a LIVE, visible boss. Past DC_ENGAGE_RANGE (the
    // top-of-Execute hold handles closer than that) but within line-of-sight and
    // DC_DIRECT_PURSUIT_RANGE, walk straight at the boss's current position with
    // a per-tick re-path (MoveTo dedups, so a roughly-stationary boss gets one
    // smooth glide; a wandering boss is re-targeted as it moves — the same way
    // combat chase tracks a target). This is what stops the tank parking at the
    // static spawn anchor and waiting for the boss to wander back. A boss out of
    // LOS (around a corner) or farther than the pursuit range falls through to
    // the wall-screened long-path below.
    if (liveBoss && engageDist <= DC_DIRECT_PURSUIT_RANGE && bot->IsWithinLOSInMap(liveBoss))
    {
        // Drop any stale long-path escort glide so it doesn't keep driving the
        // bot toward the spawn anchor while we steer toward the live boss.
        StopActiveSplineGlide(bot);
        bool const chasing = MoveTo(next->mapId, bossX, bossY, bossZ,
                                    /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                    /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] pursuing live {} at {:.0f}yd (LOS) -> MoveTo {}",
                  bot->GetName(), next->name, engageDist, chasing ? "issued" : "noop/queued");
        stuck = 0;
        ClearStall(context);
        return chasing;
    }

    // Build or refresh the long-path cache. Boss positions are static,
    // so the TTL can be generous; cache is invalidated on boss change.
    EnsureLongPath(bot, context, *next);

    ChunkedPathfinder::Result const& path =
        AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");

    if (!path.reachable)
    {
        // Bot wedged off the navmesh — try a small offset to land on a
        // walkable poly. Common cause: stuck-teleport recovery landed
        // on a ledge that pad's mmap tile-boundary; another cause is
        // bot getting knocked back onto unwalkable geometry.
        if (DC_ALLOW_RECOVERY_MOVES && path.startFarFromPoly)
        {
            if (TryFarFromPolyRecovery(bot))
            {
                // Don't say anything in party chat — this should be
                // invisible recovery. Force a rebuild so the next tick
                // picks up the new (hopefully on-mesh) position.
                context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
                return true;
            }
        }

        // The chunked builder couldn't produce any segment. Failure
        // reason is carried through from PathGenerator's path type
        // (NOPATH, FARFROMPOLY_START, etc.). The stalled-fallback action
        // takes over from here, picking off whatever reachable hostiles
        // remain to potentially unblock the route.
        StallDungeonClear(botAI,
            "Can't path to " + next->name + ": " +
            (path.failureReason.empty() ? "no navigable route" : path.failureReason) +
            ". I'll try to clear intervening mobs; if that doesn't help, 'dc skip' to move on.");
        return false;
    }

    DungeonFollowerState& follower =
        context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get();

    // On-path tracking: if the bot has drifted off the planned corridor
    // (knockback, charge, sticky-trash detour, follower bump), try a
    // Resnap onto the existing polyline before issuing the next hop.
    // Resnap failure means we're too far for a safe index-jump — force
    // a rebuild from the bot's current position and yield this tick so
    // the next tick comes back with a fresh path.
    if (DungeonPathFollower::IsOffPath(bot, path, follower) &&
        follower.offPathTicks >= DungeonPathFollower::OFF_PATH_TICK_LIMIT)
    {
        uint32 const offTicks = follower.offPathTicks;  // Resnap zeroes this
        if (!DungeonPathFollower::Resnap(bot, path, follower))
        {
            // Drift too large to index-jump. Halt any stale spline glide so
            // the rebuilt path isn't shadowed by the old route next tick.
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] off-path {} ticks, Resnap FAILED (>{}yd) -> rebuild",
                     bot->GetName(), offTicks, DungeonPathFollower::RESNAP_RADIUS);
            StopActiveSplineGlide(bot);
            context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
            follower = DungeonFollowerState{};
            return false;
        }
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] off-path {} ticks -> Resnapped to seg {} pt {}",
                  bot->GetName(), offTicks, follower.segmentIdx, follower.pointIdx);
    }

    DungeonPathFollower::Hop hop = DungeonPathFollower::NextHop(bot, path, follower);

    // Post-combat re-anchor. NextHop only fast-forwards the cursor past points
    // the tank passed within POINT_REACHED; a trash chase displaces it well
    // off those points — often FORWARD along the route — leaving the cursor
    // stale and behind, so the tank would walk backward to it. If the next hop
    // is implausibly far for normal gliding, re-anchor onto the nearest
    // visible route point (Resnap is LOS-gated, so it won't snap across a
    // wall) and re-fetch the hop. This is the "fight, then re-follow the path"
    // step. On Resnap failure the tank is genuinely far from the route — fall
    // through and let the off-path/stuck logic rebuild from here.
    if (!hop.isDone && !hop.isJump &&
        bot->GetDistance(hop.point.x, hop.point.y, hop.point.z) > DC_REANCHOR_DISTANCE)
    {
        float const staleDist = bot->GetDistance(hop.point.x, hop.point.y, hop.point.z);
        bool const reanchored = DungeonPathFollower::Resnap(bot, path, follower);
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] re-anchor: next hop {:.1f}yd (>{}yd, stale cursor) -> {}",
                  bot->GetName(), staleDist, DC_REANCHOR_DISTANCE,
                  reanchored ? "Resnapped + refetched hop" : "Resnap failed, falling through");
        if (reanchored)
            hop = DungeonPathFollower::NextHop(bot, path, follower);
    }

    // Sync the legacy "current hop" telemetry — `dc status` and a few
    // tests still read it. Map the flattened polyline cursor onto its
    // owning segment index.
    context->GetValue<uint32>("dungeon clear current hop")->Set(follower.segmentIdx);
    if (hop.isDone)
    {
        // Cursor reached the polyline end. If we're already within engage range
        // this is a benign "anchored hops were still pending at the top" case —
        // rebuild and let the engage hold take over next tick.
        if (engageDist < DC_ENGAGE_RANGE)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] reached end of path polyline (seg {}) -> forcing rebuild next tick",
                     bot->GetName(), follower.segmentIdx);
            context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
            return false;
        }

        // The route dead-ends short of the boss. Rebuilding here just produces
        // the same 0-point path next tick (we're sitting on its terminal poly),
        // and since the bot isn't moving the posStuck counter never escalates —
        // this is the silent forever-loop. Try a straight final-approach MoveTo
        // first: PathGenerator may close a few yards the chunk builder gave up
        // on, or the boss may have wandered into reach. Past the retry budget,
        // declare it unreachable and stall for `dc skip`.
        ++doneNotEngagedTicks;
        if (doneNotEngagedTicks < DC_DONE_NOT_ENGAGED_LIMIT)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] path ends {:.0f}yd short of {} (>{:.0f}, attempt {}/{}) "
                     "-> final-approach MoveTo",
                     bot->GetName(), engageDist, next->name, DC_ENGAGE_RANGE,
                     doneNotEngagedTicks, DC_DONE_NOT_ENGAGED_LIMIT);
            StopActiveSplineGlide(bot);
            bool const pushing = MoveTo(next->mapId, bossX, bossY, bossZ,
                                        /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                        /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);
            context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
            return pushing;
        }

        doneNotEngagedTicks = 0;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] {} unreachable: route dead-ends {:.0f}yd short after {} approach "
                 "attempts -> stalling",
                 bot->GetName(), next->name, engageDist, DC_DONE_NOT_ENGAGED_LIMIT);
        StallDungeonClear(botAI,
            "Can't reach " + next->name + ": the route dead-ends short of it "
            "(likely on a ledge or across a gap the navmesh doesn't span). "
            "Use 'dc skip' to move to the next boss.");
        return false;
    }

    // Anchor-declared jumps: use JumpTo (MotionMaster::MoveJump) instead
    // of MoveTo. Required for dungeon drop-downs the mmap doesn't model
    // (OK upper→lower, Pinnacle Skadi catwalk, AN spider tunnels, etc.).
    // The jump's destination still goes through the engine's movement
    // pipeline; if MoveJump arcs short, the next tick re-evaluates.
    if (hop.isJump)
    {
        bool const jumped = JumpTo(next->mapId, hop.point.x, hop.point.y, hop.point.z,
                                   MovementPriority::MOVEMENT_NORMAL);
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] jump leg -> ({:.1f},{:.1f},{:.1f}) {}",
                  bot->GetName(), hop.point.x, hop.point.y, hop.point.z,
                  jumped ? "issued" : "JumpTo refused (higher-prio move in flight), retry");
        if (!jumped)
        {
            // JumpTo can return false if a previous move with equal/higher
            // priority is still in flight. Don't count this as a stall —
            // try again next tick. Position-based stuck detection covers
            // the case where the jump truly never lands.
            return false;
        }
        ClearStall(context);
        return true;
    }

    // --- Continuous-spline advance ---------------------------------------
    // Rather than issue one short MoveTo per Advance tick — which stops the
    // bot dead at every ~8yd polyline point and idles it until the next tick
    // re-fires (the "step, pause 2-3s, step" stutter) — hand the whole
    // upcoming polyline run to the core as ONE EscortMovementGenerator
    // spline. The core then walks it server-side every frame, exactly like
    // MoveFollow does for a leader, so the bot glides continuously instead
    // of restarting from a standstill each tick.
    //
    // The escort generator builds a LINEAR spline (it never calls SetSmooth),
    // so the bot tracks the LOS-screened polyline segment-for-segment with no
    // Catmull-Rom corner-cutting — preserving the wall-safety the per-point
    // MoveTo gave us, without the per-point stops.
    MotionMaster* mm = bot->GetMotionMaster();

    // Leave a healthy in-flight spline alone. NextHop above already advanced
    // the follower cursor past the points we've glided over, so there's
    // nothing to do but let it ride. Re-issuing now would StopMoving and
    // relaunch (a visible hitch) — the very stutter we're removing. The
    // IsWaitingForLastMove guard bridges the sub-tick window between issuing
    // the spline and isMoving() reading true, so we never double-launch.
    bool const splineRunning =
        mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE && bot->isMoving();
    if (splineRunning || IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        stuck = 0;
        ClearStall(context);
        return true;
    }

    if (!IsMovingAllowed())
        return false;

    // Build the spline window from the current cursor. It stops before any
    // jump leg (handled by the JumpTo branch above on a later tick) and at
    // MAX_SPLINE_WINDOW_POINTS. window[0] is the live position; [1..] are
    // the polyline points to glide through.
    std::vector<G3D::Vector3> const window =
        DungeonPathFollower::BuildSplineWindow(bot, path, follower);
    if (window.size() >= 2 && mm)
    {
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        if (bot->IsNonMeleeSpellCast(true))
        {
            bot->CastStop();
            botAI->InterruptSpell();
        }

        Movement::PointsArray points(window.begin(), window.end());
        mm->MoveSplinePath(&points, FORCED_MOVEMENT_NONE);

        // Record NORMAL-priority movement so AttackAction::Attack still clears
        // the spline when a patrol aggros mid-glide (its interrupt gate is
        // priority < MOVEMENT_COMBAT).
        //
        // The delay MUST track the spline's real travel time, not a constant.
        // The re-issue guard above is
        //   if (splineRunning || IsWaitingForLastMove(NORMAL)) return;
        // and IsWaitingForLastMove stays true for `delay` ms. A fixed 1000ms
        // delay was a latent stutter: whenever a window is short enough that
        // the bot finishes gliding it in under a second (a near-route-end leg,
        // an LOS-truncated chunk, or a short strided chunk), the spline
        // finalizes and the bot stops, yet IsWaitingForLastMove keeps blocking
        // the next issuance until the second is up — so the tank idles ~0.3-0.5s
        // before the next glide. Glide-idle-glide-idle is the "step, pause,
        // step" the tank showed, and the idle ending is the "snap forward".
        // Sizing the delay to the window's path length makes the wait expire
        // exactly when the spline does, so the next window issues immediately
        // and the legs chain seamlessly. (Launch() sets MOVEMENTFLAG_FORWARD
        // synchronously, so isMoving()/splineRunning is already true on the
        // next tick — there is no issue→moving gap that needed a fixed bridge.)
        float windowLen = 0.0f;
        for (size_t i = 1; i < window.size(); ++i)
            windowLen += (window[i] - window[i - 1]).magnitude();
        float const runSpeed = std::max(0.1f, bot->GetSpeed(MOVE_RUN));
        float delay = 1000.0f * (windowLen / runSpeed);
        delay = std::min(delay, static_cast<float>(sPlayerbotAIConfig.maxWaitForMove));
        delay = std::max(delay, static_cast<float>(sPlayerbotAIConfig.reactDelay));

        G3D::Vector3 const& dest = window.back();
        AI_VALUE(LastMovement&, "last movement")
            .Set(next->mapId, dest.x, dest.y, dest.z, bot->GetOrientation(), delay,
                 MovementPriority::MOVEMENT_NORMAL);

        // The cadence of these lines IS the step-pause signature: with the
        // timestamp prefix, consecutive issues spaced ~= their own `delay`
        // mean seamless chaining; a gap much larger than `delay` between a
        // short window's issue and the next is the dead-pause to investigate.
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] spline issued: {} pts, {:.1f}yd, speed={:.1f}, delay={:.0f}ms "
                  "(seg {} pt {})",
                  bot->GetName(), window.size(), windowLen, runSpeed, delay,
                  follower.segmentIdx, follower.pointIdx);

        stuck = 0;
        ClearStall(context);
        return true;
    }

    // Fallback: the next leg is a jump or a lone anchor tail (window < 2
    // points), so spline issuance isn't possible. Issue the single short hop
    // — short enough that the engine's per-MoveTo re-pathfind never trips
    // PATHFIND_SHORT, the same wall-safety the spline path preserves.
    // MOVEMENT_NORMAL (not COMBAT) so combat reflexes can still preempt it.
    LOG_DEBUG("playerbots.dungeonclear",
              "[DC:{}] spline window <2 pts -> per-point MoveTo fallback to "
              "({:.1f},{:.1f},{:.1f}) (seg {} pt {})",
              bot->GetName(), hop.point.x, hop.point.y, hop.point.z,
              follower.segmentIdx, follower.pointIdx);
    bool const moved = MoveTo(next->mapId, hop.point.x, hop.point.y, hop.point.z,
                              /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                              /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);
    if (!moved)
    {
        // MoveTo returned false. Benign in the common case (duplicate
        // move queued / waiting on last move). Only treat as a real
        // stall after several consecutive failures.
        if (++stuck >= DC_STUCK_LIMIT)
        {
            // Force a fresh chunked rebuild — the cached path's first
            // segment may be unreachable from our actual current poly.
            context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
            follower = DungeonFollowerState{};
            StallDungeonClear(botAI,
                "Stuck near " + next->name + " — I have a path but movement isn't progressing. "
                "I'll try to clear nearby mobs; use 'dc skip' if it persists.");
            return false;
        }
        return false;
    }

    stuck = 0;
    ClearStall(context);
    return true;
}

namespace
{
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
            if (!DungeonClearUtil::IsLevelReachable(bot, u))
                continue;
            best = u;
            bestDist = dist;
        }
        return best;
    }
}

bool DungeonClearEngageTrashAction::Execute(Event /*event*/)
{
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
            fresh = DungeonClearUtil::FindBlockingTrashOnPath(
                bot, path.segments, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
        }
        else
        {
            // No usable long-path cache — fall back to single-shot corridor.
            Movement::PointsArray corridor;
            if (DungeonClearUtil::ComputeCorridor(bot, next->x, next->y, next->z, corridor))
            {
                fresh = DungeonClearUtil::FindBlockingTrashCorridor(
                    bot, corridor, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
            }
        }
    }
    if (!fresh)
    {
        // Final fallback: cone scan, same predicate as the trigger uses.
        // Keeps the engagement path live when path computation can't
        // produce any usable corridor (boss off-mesh, etc.).
        fresh = DungeonClearUtil::FindBlockingTrash(
            bot, *next, DC_ENGAGE_CONE_RANGE, DC_ENGAGE_CONE_HALF_ANGLE, candidates);
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
    return EngageDirect(target);
}

bool DungeonClearEngageBossAction::Execute(Event /*event*/)
{
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
        return false;

    Creature* boss = DungeonClearUtil::FindLiveCreatureOnMap(bot, next->entry);
    if (!boss)
    {
        StallDungeonClear(botAI,
            "Can't reach " + next->name + ": not spawned on this map. Use 'dc skip' to move to the next boss.");
        return false;
    }

    if (!EngageDirect(static_cast<Unit*>(boss)))
        return false;

    ClearStall(context);
    return true;
}

bool DungeonClearClearStalledAction::Execute(Event /*event*/)
{
    Unit* target = DungeonClearUtil::FindNearestReachableHostile(bot);
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
        DungeonClearUtil::SendAddonMessage(botAI, "CHAT\tClearing path \xe2\x80\x94 pulling " + std::string(target->GetName()) + ".");
    }

    // Don't clear the stall reason here — only a successful Advance does that.
    // If pathing to the boss is still blocked after this kill, we want the
    // stall trigger to fire again next tick.
    return EngageDirect(target);
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

bool DungeonClearDoorBlockedAction::Execute(Event /*event*/)
{
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    std::string const target = next.has_value() ? next->name : "the next boss";

    auto parkAndStall = [&]()
    {
        if (bot->isMoving())
            bot->StopMoving();
        StallDungeonClear(botAI,
            "A door blocks the path to " + target + ". Open it and I'll resume; 'dc off' to stop.");
        return true;
    };

    // The door is detected as much as 80yd ahead along the corridor (see
    // DungeonClearBlockingDoorValue). Walk up to it first so the tank waits
    // AT the door instead of parking wherever it happened to be when the door
    // entered look-ahead.
    ObjectGuid const doorGuid = AI_VALUE(ObjectGuid, "dungeon clear blocking door");
    GameObject* door = doorGuid.IsEmpty() ? nullptr : botAI->GetGameObject(doorGuid);
    if (!door)
    {
        // Door vanished/opened between the value poll and now, or its grid
        // isn't resident — fall back to the in-place stall so the reason is
        // still reported.
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] door-blocked: door guid {} unresolved -> parking in place",
                 bot->GetName(), doorGuid.ToString());
        return parkAndStall();
    }

    float const distToDoor = bot->GetExactDist(door);
    if (distToDoor > DC_DOOR_STOP_DISTANCE)
    {
        // A stale escort glide from Advance would keep driving the OLD boss
        // route (past where the door sits) and the splineRunning guard would
        // mistake it for healthy movement — drop it before issuing our MoveTo.
        StopActiveSplineGlide(bot);
        // NORMAL priority so a hostile aggro'd en route still interrupts the
        // walk-in and gets fought (engage triggers outrank this action anyway).
        bool const moving = MoveTo(door, DC_DOOR_STOP_DISTANCE, MovementPriority::MOVEMENT_NORMAL);
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] door-blocked: walking to door at {:.1f}yd (stop {:.0f}yd) moved={}",
                  bot->GetName(), distToDoor, DC_DOOR_STOP_DISTANCE, moving ? 1 : 0);
        if (moving)
            return true;
        // MoveTo refused (already in range per its own threshold, or no path)
        // — fall through and park where we are.
    }

    LOG_DEBUG("playerbots.dungeonclear",
              "[DC:{}] door-blocked: at door ({:.1f}yd) -> parking, waiting for it to open",
              bot->GetName(), distToDoor);
    return parkAndStall();
}

bool DungeonClearFollowTankAction::Execute(Event /*event*/)
{
    ObjectGuid& followedTank =
        context->GetValue<ObjectGuid>("dungeon clear followed tank")->RefGet();

    Player* tank = AI_VALUE(Player*, "dungeon clear party tank");
    if (!tank || tank == bot)
    {
        // No DC tank: tear down the leftover continuous MoveFollow we
        // installed while following. MoveFollow is a persistent MotionMaster
        // order; once the tank's DC flag clears, this action stops being
        // selected and nothing else cancels it for a self-bot (its ordinary
        // follow targets itself and no-ops without clearing), so it would
        // stay glued to the tank. Clear it once, forget the tank, and let the
        // bot revert to stock behavior (a self-bot then stands still as the
        // leader; normal bots fall back to following their master).
        if (!followedTank.IsEmpty())
        {
            if (bot->GetMotionMaster() &&
                bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            {
                if (bot->isMoving())
                    bot->StopMoving();
                bot->GetMotionMaster()->Clear();
            }
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] follow-tank: released (DC tank gone) -> cleared "
                     "follow generator (selfRealPlayer={})",
                     bot->GetName(), botAI && botAI->IsRealPlayer() ? 1 : 0);
            followedTank = ObjectGuid::Empty;
        }
        return false;
    }

    // Loot yield (with commit-timeout). Followers run ONLY this action while DC
    // is active (their own DC is never enabled — `dc on` is tank-only — so the
    // advance/engage triggers are all inactive for them; follow-tank, relevance
    // 25, outranks the loot actions (open loot 8, move to loot 7, loot 6) every
    // non-combat tick). Without yielding here, the instant `move to loot` walks
    // a follower past followDistance toward a corpse, follow-tank wins the next
    // tick and yanks it straight back — so followers can only ever pick up
    // corpses already sitting inside the tank's follow bubble, and a corpse a
    // few yards out never gets looted at all. The tank pauses between pulls to
    // let loot resolve (see the advance loot-yield in Execute); mirror that here
    // so the party can actually path out to corpses during that window.
    //
    // Stepping aside on EITHER loot flag covers the whole pickup lifecycle:
    // "has available loot" is true only at ~3-15yd and flips FALSE at ~3yd when
    // "can loot" flips TRUE, so yielding on just one would re-assert follow at
    // the 3yd boundary and pull the bot off the corpse before `open loot` ran
    // — the same oscillation the tank's advance yield avoids. The stack is
    // populated by `add all loot` (relevance 5), which gets its tick whenever
    // the follower is already within followDistance and Follow() no-ops (returns
    // false) — i.e. while clustered on the resting tank, exactly when corpses
    // are nearby. The timeout stops a follower parking forever on a corpse it
    // can't finish (group-roll pending, bags full) and being left behind, which
    // would also stall the tank on its party-spread gate.
    uint32& lootYieldStart =
        context->GetValue<uint32>("dungeon clear loot yield start")->RefGet();
    bool const lootYield =
        AI_VALUE(bool, "has available loot") || AI_VALUE(bool, "can loot");
    if (lootYield)
    {
        uint32 const now = getMSTime();
        if (lootYieldStart == 0)
            lootYieldStart = now;

        if (now - lootYieldStart < DC_LOOT_YIELD_TIMEOUT_MS)
        {
            // Hand the tick to move-to-loot / open-loot. Don't issue a follow
            // move that would drag the bot off the corpse.
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] follow-tank yielding: loot in progress ({}ms)",
                      bot->GetName(), now - lootYieldStart);
            return false;
        }
        // Waited long enough — give up on this corpse and resume following so
        // we rejoin the tank. Leave lootYieldStart expired so we keep following
        // each tick until the loot flags clear (which resets the timer).
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] follow-tank loot-yield timed out after {}ms -> resuming follow",
                 bot->GetName(), now - lootYieldStart);
    }
    else
    {
        lootYieldStart = 0;  // not looting -> reset the commit timer
    }

    // Tighter cluster than default. Keeps followers in healer LOS and out
    // of mob aggro-radius arcs during the advance. Default followDistance
    // (~10yd) had them strung out by the time the tank engaged.
    float const dist = std::min<float>(sPlayerbotAIConfig.followDistance, 6.0f);
    // Remember who we're chasing so the teardown branch above can cancel this
    // continuous MoveFollow once the DC tank goes away.
    followedTank = tank->GetGUID();
    return Follow(tank, dist);
}
