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
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproach.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproachIo.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
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

namespace DcActionShared
{

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


    // Resolve PickPullSpell to a usable spell id, but ONLY if the tank itself
    // trained it. botAI->CastSpell(name) is NOT a sufficient gate on its own:
    // the "spell id" value falls back to the bot's PET spellbook when the bot
    // doesn't know the spell, and CastSpell then builds the spell on the bot
    // regardless of knowledge — i.e. the server quietly FORCES the tank to "cast"
    // a pull it never learned. Resolving the id here and requiring
    // bot->HasSpell(id) (player-only; never the pet) means an untrained tank
    // returns nullopt and just walks in to body-tag instead.
    std::optional<ResolvedPullSpell> ResolvePullSpell(PlayerbotAI* botAI, Player* bot)
    {
        if (!botAI || !bot)
            return std::nullopt;
        auto pick = PickPullSpell(bot);
        if (!pick)
            return std::nullopt;
        uint32 const spellId =
            botAI->GetAiObjectContext()->GetValue<uint32>("spell id", pick->name)->Get();
        if (!spellId || !bot->HasSpell(spellId))
            return std::nullopt;
        return ResolvedPullSpell{spellId, pick->minRange, pick->maxRange};
    }


    void DisableDungeonClear(PlayerbotAI* botAI, std::string const& reason)
    {
        AiObjectContext* ctx = botAI->GetAiObjectContext();
        Player* bot = botAI->GetBot();
        ctx->GetValue<bool>("dungeon clear enabled")->Set(false);
        if (bot)
            DcStatusPublisher::UnmarkActiveTank(bot->GetGUID());
        ctx->GetValue<bool>("dungeon clear paused")->Set(false);
        ctx->GetValue<std::string&>("dungeon clear pause reason")->Get().clear();
        ctx->GetValue<ObjectGuid>("dungeon clear paused door")->Set(ObjectGuid::Empty);
        ctx->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
        ctx->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
        ctx->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
        ctx->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
        ctx->GetValue<std::string&>("dungeon clear phase")->Get().clear();
        ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
        ctx->GetValue<std::map<ObjectGuid, uint32>&>("dungeon clear loot skip")->Get().clear();
        ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
        ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};
        // One reset clears the whole approach FSM (the stuck/recovery counters,
        // the pursuit/dead-end latches, the loot-yield anchor, the position
        // sentinel + committed-boss entry, and the long-path cache state) in
        // lockstep — see DcApproachState.
        ctx->GetValue<DcApproachState&>("dungeon clear approach state")->Get().Reset();
        // One reset clears the whole advanced-pull FSM (phase / dwell timer / camp /
        // breadcrumb trail / abort + tag latches / Dynamic verdict) in lockstep.
        ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get().Reset();
        // Pull-session teardown, previously only on the chat `dc off` path: revert
        // the pull preference to Dynamic, disarm the behavioral bool, and REVOKE the
        // leader's pull-session daze immunity. Folding these in here is the fix for a
        // party-death / dungeon-exit disable (which routes through this function)
        // leaving the tank permanently daze-immune — the revoke was formerly
        // reachable only via ApplyPullSetting and DcOffAction.
        ctx->GetValue<uint32>("dungeon clear pull setting")->Set(2u);
        ctx->GetValue<bool>("dungeon clear pull mode")->Set(false);
        if (bot)
            DcLeaderSignal::SetLeaderDazeImmunity(bot, false);
        DcStatusPublisher::SendAddonMessage(botAI, "CHAT\t" + reason);
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
            DcStatusPublisher::SendAddonMessage(botAI, "CHAT\t" + reason);
            botAI->DoSpecificAction("dc status", Event(), true);
        }
    }


    void ClearStall(AiObjectContext* ctx)
    {
        ctx->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
        ctx->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    }


    // Record the navigation micro-activity for this advance tick so the ~2s
    // status poll can report a fine-grained state to the addon (see
    // DungeonClearPhaseValue / DcStatusAction). Cheap string assignment; called
    // from each terminal movement/recovery branch of Advance::Execute. Tokens:
    // "moving", "pursuing", "recovering".
    void SetPhase(AiObjectContext* ctx, std::string const& phase)
    {
        ctx->GetValue<std::string&>("dungeon clear phase")->Get() = phase;
    }


    // Party-readiness gate for resuming the advance (HP/MP/spread). Shared body
    // in DcPartyState (one implementation with the trigger ladder, which had
    // drifted as two copies). requireNoLoot is false here: loot is handled
    // separately in Execute so it can enforce a commit-timeout; see the
    // loot-yield block there.
    bool IsBetweenPullsReady(Player* bot, AiObjectContext* context)
    {
        // Memoised within the tick (loose variant); see DcTickMemo.
        return DcTickMemoAccess::BetweenPullsReady(bot, context, /*requireNoLoot*/ false);
    }


    // Commit a freshly-built path into the cache and reset the follower so we
    // don't index off the end of a shorter polyline. Shared by the sync and
    // async install sites.
    // `builtToward` is the position the route was actually built to reach; the
    // retarget baseline (longPathTargetPos, measured by EnsureLongPath's `moved`
    // drift check) must be stamped from it, NOT the current-tick target coords.
    // They differ on the async path: the route was built toward the submit-time
    // coords (raw.tx/ty/tz) while `target` may have drifted during the pending
    // window (a patrolling boss). nullptr = built toward the target's live coords
    // (all synchronous sites).
    void InstallLongPath(Player* bot, AiObjectContext* ctx, DcApproachState& appr,
                         DungeonBossInfo const& target,
                         ChunkedPathfinder::Result&& built, uint32 now, char const* how,
                         Position const* builtToward = nullptr)
    {
        ChunkedPathfinder::Result& path =
            ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Get();
        path = std::move(built);
        appr.longPathTargetEntry = target.entry;
        appr.longPathTargetPos = builtToward ? *builtToward : Position(target.x, target.y, target.z);
        appr.longPathExpiresMs = now + DC_LONG_PATH_TTL_MS;

        size_t const firstSegPts = path.segments.empty() ? 0 : path.segments.front().polyline.size();
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] path rebuilt -> {} ({}): segs={} firstSegPts={} complete={}{}",
                 bot->GetName(), target.name, how,
                 path.segments.size(), firstSegPts, path.complete,
                 path.reachable ? "" : (" UNREACHABLE: " + path.failureReason));

        ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
        ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

        // Re-baseline the TTL-defer progress cursor to the freshly-reset follower
        // (segment 0, point 0) so the first real advance reads as progress.
        appr.lastProgressSegmentIdx = 0;
        appr.lastProgressPointIdx = 0;
    }


    // Rebuild the cached long-path when the boss entry changes or the TTL
    // expires. Idempotent — safe to call every Advance tick.
    //
    // With DungeonClear.AsyncPathfinding ON (default) the heavy navmesh A* runs
    // on DcPathWorker's background thread so it can't micro-stutter the map
    // tick. This call then becomes: drain a finished job and install it, or (if
    // a rebuild is due and nothing is in flight) submit one and keep serving the
    // current path until it lands. Boss changes briefly clear the cache (no
    // valid path for the new target exists yet); TTL refreshes keep walking.
    //
    // Hand-tuned anchor routes are cheap (snap-only, no A*) and must take
    // precedence over the LongRange corridor, so when one is registered for the
    // boss we build synchronously and skip the worker entirely. The worker only
    // ever runs LongRangePathfinder::BuildCoreFromMesh.
    void EnsureLongPath(Player* bot, AiObjectContext* ctx, DcApproachState& appr,
                        DungeonBossInfo const& target)
    {
        uint32& cachedEntry = appr.longPathTargetEntry;
        uint32& expiresAt = appr.longPathExpiresMs;
        uint64& pendingJob = appr.pendingPathJob;
        uint32& pendingSince = appr.pendingPathSinceMs;
        uint32 const now = getMSTime();

        bool const asyncEnabled = DcSettings::GetBool(bot, "AsyncPathfinding");

        // ---- 1. Drain a completed async build ----
        if (pendingJob)
        {
            LongRangePathfinder::RawResult raw;
            uint32 jobEntry = 0;
            uint32 jobMap = 0;
            if (!DcPathWorker::Instance().TryTake(pendingJob, raw, jobEntry, jobMap))
            {
                // No result yet. A real build is a single sub-tick Detour query,
                // so a multi-second wait means the result was lost (swept by the
                // reaper after an afk dc-off, since not every reset path clears
                // the pending jobId) or the worker is wedged/dead. Abandon the
                // job and rebuild synchronously this tick — otherwise step 1
                // short-circuits forever and no toggle/skip can recover it.
                if (getMSTimeDiff(pendingSince, now) < DC_ASYNC_PATH_PENDING_TIMEOUT_MS)
                    return;  // still building — keep serving the cached path

                // INFO so it's trackable: this is the only path that runs a full
                // long-range build on the world thread in async mode, which is
                // exactly what async exists to avoid. It should be rare (lost
                // result / wedged worker); frequent hits mean the worker is
                // unhealthy and want investigating.
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] ASYNC PATH SYNC-FALLBACK: job {} gave no result in {}ms — "
                         "abandoning and rebuilding on the world thread (worker swept/stalled?)",
                         bot->GetName(), pendingJob, getMSTimeDiff(pendingSince, now));
                pendingJob = 0;
                pendingSince = 0;
                ChunkedPathfinder::Result built =
                    ChunkedPathfinder::Build(bot, target.mapId, target.entry, target.x, target.y, target.z);
                InstallLongPath(bot, ctx, appr, target, std::move(built), now, "sync (async timeout)");
                return;
            }

            pendingJob = 0;
            pendingSince = 0;

            // Discard a result the world has moved past (boss changed, or the
            // bot zoned) — installing it would route toward the wrong target.
            bool const stale = (jobEntry != target.entry) || (jobMap != bot->GetMapId());
            if (!stale)
            {
                ChunkedPathfinder::Result built = LongRangePathfinder::Finalize(bot, raw);
                if (!built.reachable && !built.startFarFromPoly)
                {
                    // LongRange couldn't reach the boss; run the lighter
                    // anchor/bee-line/arc/spawn-graph fallback tiers on the map
                    // thread WITHOUT redoing the heavy A* we already offloaded.
                    built = StridedPathfinder::Build(bot, target.mapId, target.entry,
                                                     target.x, target.y, target.z, 16, /*skipLongRange*/ true);
                }
                // Stamp the retarget baseline from the coords the route was BUILT
                // toward (raw.tx/ty/tz), not target's possibly-drifted live coords,
                // so EnsureLongPath's `moved` check measures live-boss drift against
                // the right anchor.
                Position const builtToward(raw.tx, raw.ty, raw.tz);
                InstallLongPath(bot, ctx, appr, target, std::move(built), now, "async", &builtToward);
                return;
            }
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] discarded stale async path (built for boss {} map {}, now boss {} map {})",
                      bot->GetName(), jobEntry, jobMap, target.entry, bot->GetMapId());
            // fall through to re-evaluate need and maybe resubmit
        }

        // ---- 2. Is a (re)build due? ----
        bool const targetChanged = cachedEntry != target.entry;
        // The live boss relocated (pool/wandering boss) or its live position
        // just streamed in far from the static anchor the current path was
        // built toward — retarget early instead of walking a stale route the
        // full TTL. Only meaningful once a path actually exists (expiresAt!=0).
        Position const& builtToward = appr.longPathTargetPos;
        bool const moved = expiresAt != 0 &&
            builtToward.GetExactDist(Position(target.x, target.y, target.z)) >
                DC_LONG_PATH_RETARGET_DIST;

        // TTL rebuild, deferred while the follower is visibly progressing. The
        // three reasons the TTL is kept short are all now covered out-of-band:
        // live-boss drift by `moved` above, boss change by `targetChanged`, and
        // stuck recovery by the expiresAt=0 forced invalidations. So a bare TTL
        // expiry on a route the bot is actively walking only triggers a full
        // A* + Finalize rebuild of a perfectly good path (and resets the
        // follower cursor). Honour the TTL only once forward progress has
        // stalled: while the cursor has advanced past the last baseline AND the
        // bot isn't position-stuck, treat the route as fresh and re-arm the
        // deadline from now. expiresAt==0 (no usable path yet) always rebuilds.
        DungeonFollowerState const& follower =
            ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get();
        bool const cursorAdvanced =
            follower.segmentIdx > appr.lastProgressSegmentIdx ||
            (follower.segmentIdx == appr.lastProgressSegmentIdx &&
             follower.pointIdx > appr.lastProgressPointIdx);
        bool const progressing = cursorAdvanced && appr.posStuckTicks == 0;
        bool const ttlPassed = expiresAt != 0 && now >= expiresAt;
        bool const expired = expiresAt == 0 || (ttlPassed && !progressing);

        if (!targetChanged && !expired && !moved)
        {
            // Not rebuilding this tick. If we deferred a lapsed TTL because the
            // bot is progressing, re-arm the deadline and roll the progress
            // baseline forward so the backstop measures the NEXT window and only
            // fires a full TTL after progress actually stalls.
            if (ttlPassed && progressing)
            {
                expiresAt = now + DC_LONG_PATH_TTL_MS;
                appr.lastProgressSegmentIdx = follower.segmentIdx;
                appr.lastProgressPointIdx = follower.pointIdx;
            }
            return;
        }

        // ---- 3. Anchor route OR sync mode → build inline ----
        // Anchor-route lookup is O(1)-ish and navmesh-only; a registered route
        // means the synchronous build is cheap (no A*), so there's nothing to
        // offload. Sync mode (toggle OFF) always builds inline.
        Map* map = bot->GetMap();
        bool hasAnchorRoute = false;
        if (map)
            hasAnchorRoute = DungeonClearRouteRegistry::Get(target.mapId, map->GetDifficulty(),
                                                            target.entry) != nullptr;

        if (!asyncEnabled || hasAnchorRoute)
        {
            ChunkedPathfinder::Result built =
                ChunkedPathfinder::Build(bot, target.mapId, target.entry, target.x, target.y, target.z);
            InstallLongPath(bot, ctx, appr, target, std::move(built), now,
                            !asyncEnabled ? "sync" : "sync (anchor route)");
            return;
        }

        // ---- 4. Async submit ----
        // On a true boss change there is no valid path for the new target, so
        // clear the cache + follower (Advance stalls briefly until the job
        // lands). On a TTL-only refresh leave the cache walking.
        if (targetChanged)
        {
            ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
            cachedEntry = target.entry;   // stop re-detecting "boss changed" every tick
            expiresAt = 0;                // mark "no usable path yet"
            ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
            ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};
        }

        // One in-flight job per bot; nothing to do until it returns.
        if (pendingJob)
            return;

        // Pin the navmesh alive across the worker round-trip. If the map has no
        // navmesh, fall back to a synchronous build so the bot never wedges.
        std::shared_ptr<dtNavMesh> meshRef =
            map ? map->GetMapCollisionData().GetMMapNavMeshSharedPtr() : std::shared_ptr<dtNavMesh>();
        if (!meshRef)
        {
            ChunkedPathfinder::Result built =
                ChunkedPathfinder::Build(bot, target.mapId, target.entry, target.x, target.y, target.z);
            InstallLongPath(bot, ctx, appr, target, std::move(built), now, "sync (no navmesh)");
            return;
        }

        pendingJob = DcPathWorker::Instance().Submit(
            bot->GetMapId(), target.entry, bot->GetGUID(), std::move(meshRef),
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            target.x, target.y, target.z);
        pendingSince = now;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] async path submitted job={} -> {} ({})",
                  bot->GetName(), pendingJob, target.name, targetChanged ? "boss changed" : "TTL");
    }


    // The escort-spline stop primitives (StopActiveSplineGlide / HaltForHold)
    // now live in DcMovement, the single movement funnel. Call sites use
    // DcMovement::StopBot(bot, Stop::Hold) (the old HaltForHold), the
    // DcMoveTo funnel (which folds in the escort-conflict teardown), or
    // DcMovement::ResolveEscortConflict (a bare glide-kill before a non-move).

    // Face `unit` if we're meaningfully off-axis. One facing packet when needed,
    // none when already facing (the HasInArc guard keeps it idempotent across a
    // multi-second hold), so it is safe to call every hold tick. Used at the
    // pull's park-and-wait sites: a tank parked side-on to the pack it is about
    // to pull, or a party staring in random directions at camp, are instant bot
    // tells. Never call this mid-glide — facing packets on a moving bot cause
    // rubber-banding.
    void DcFaceIfNeeded(Player* bot, Unit* unit)
    {
        if (!bot || !unit || unit == bot || bot->HasInArc(CAST_ANGLE_IN_FRONT, unit))
            return;
        ServerFacade::instance().SetFacingTo(bot, unit);
    }

}  // namespace DcActionShared

// Arbiter-funneled point move. The single seam through which DC actions issue a
// MovementAction::MoveTo: refuse while the run is paused (killing the
// queued-action race), drop a stale escort glide that would otherwise coast
// under the new move, then delegate. Same own-the-tick semantics as the inherited
// MoveTo. Lives here in Part 1; moves to DcActionShared with the file split.
bool DcMovementAction::DcMoveTo(uint32 mapId, float x, float y, float z, bool idle, bool react,
                                bool normal_only, bool exact_waypoint, MovementPriority priority,
                                bool lessDelay, bool backwards)
{
    if (!DcMovement::DcMovementAllowed(botAI))
        return false;
    DcMovement::ResolveEscortConflict(bot);
    return MoveTo(mapId, x, y, z, idle, react, normal_only, exact_waypoint, priority, lessDelay,
                  backwards);
}
