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
    //
    // This threshold is PER TICK and must stay well below the distance a
    // HEALTHY escort glide covers in one Advance tick, or normal movement is
    // misread as stuck. With run speed ~7 yd/s and the Advance cadence of
    // ~0.2 s, a gliding bot moves ~1.4-1.5 yd/tick — so the old 1.5 yd value
    // flagged EVERY healthy tick, tripped the limit in 5 ticks, and killed the
    // bot's own good spline (the stutter-step through hallways/doors: glide a
    // few yards, false-stuck, stop, idle, re-issue, repeat). A genuinely
    // wedged bot moves ~0 yd/tick, so 0.5 yd cleanly separates the two: it is
    // 3x above the wedge floor yet ~1 yd below the slowest healthy glide.
    constexpr float DC_STUCK_DISPLACEMENT = 0.5f;
    constexpr uint32 DC_STUCK_TICK_LIMIT = 5;

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

    // Consecutive direct-pursuit ticks that issued no movement (MoveTo returned
    // false and the bot is neither moving nor waiting on an in-flight move)
    // before Advance abandons the LIVE-boss direct-pursuit shortcut and falls
    // through to the wall-screened long-path. The direct-pursuit MoveTo bee-lines
    // the boss's live poly through the raw PathGenerator, which can fail to
    // resolve a path (Z -> INVALID_HEIGHT, or a winding route past its 74-hop
    // cap) and then silently returns false every tick — the bot never moves, so
    // the position-based stuck counter can't catch it. A short grace absorbs a
    // transient miss (boss mid-step, grid still settling); past it we hand off
    // to the long-path (LongRangePathfinder, no hop cap), which carries its own
    // dead-end -> stall escalation. ~5 ticks ≈ a couple of seconds.
    constexpr uint32 DC_PURSUIT_FAIL_LIMIT = 5;

    // Long-path cache TTL. Most bosses hold a fixed position, so a longer TTL
    // is safe; rebuild costs are bounded (~8 PathGenerator calls × sub-ms each).
    // Keeping the TTL short keeps stale paths from outlasting edge cases like
    // portal traversal or stuck-teleport recovery. A boss that relocates within
    // the TTL is handled out-of-band by DC_LONG_PATH_RETARGET_DIST.
    constexpr uint32 DC_LONG_PATH_TTL_MS = 15 * 1000;

    // How far the effective boss target may drift from the position the cached
    // long-path was built toward before EnsureLongPath forces an early rebuild,
    // ahead of the TTL. Advance feeds the boss's LIVE position when it is
    // loaded, so a pool-spawn / wandering boss (e.g. the Wailing Caverns
    // Disciples, which spawn at one of several pooled anchors and rarely sit on
    // the one BossSpawnIndex picked) gets a route to where it actually is — and
    // the moment its live position streams in far from the static anchor the
    // first build used, the path retargets instead of walking the wrong way for
    // the full TTL ("goes the wrong direction, then says the way is blocked").
    // Above minor patrol jitter so a pacing boss doesn't thrash rebuilds; the
    // direct-pursuit shortcut already handles the close-range (<=80yd, LOS)
    // tracking, so this only governs the long-range approach.
    constexpr float DC_LONG_PATH_RETARGET_DIST = 15.0f;

    // Self-healing watchdog for an in-flight async path job. A real
    // BuildCoreFromMesh is a single Detour query — sub-millisecond, well under
    // one tick — so if a submitted job has produced NO mailbox result after
    // this long, the result was lost (e.g. swept by the reaper while the bot
    // was afk with dc off, since the reset paths don't all clear the pending
    // jobId) or the worker thread is wedged/dead. Either way, abandon the
    // pending job and rebuild synchronously so the bot can never wedge forever
    // on "plotting route". Must stay well below DC_ASYNC_PATH_RESULT_TTL_MS (the
    // 30s mailbox sweep) so a genuinely-completed result is collected via
    // TryTake before this fires, and far above any real build time.
    constexpr uint32 DC_ASYNC_PATH_PENDING_TIMEOUT_MS = 5 * 1000;

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

    // How long a loot the bot abandoned (its yield above timed out on it) stays
    // on the per-corpse give-up list. While listed, DungeonClearUtil::Strip-
    // SkippedLoot keeps it out of both the loot flags and stock's nearest-target
    // pick, so the bot walks away (follow/advance) instead of re-committing to
    // it the instant it drifts back within lootDistance — the corpse<->tank /
    // chest<->path ping-pong. Long enough that the party has moved on by the
    // time it lapses; short enough to retry once in case a pending group roll
    // resolved in the meantime.
    constexpr uint32 DC_LOOT_GIVEUP_TTL_MS = 60 * 1000;

    // Camp cutoff: how long the bot may stand within interaction range of ONE
    // plain corpse (can-loot true) before we treat its loot as un-finishable and
    // skip it (see DcLootPolicy::MaybeGiveUpCampedLoot). Unlike the yield
    // timeout above — which budgets for walking in from lootDistance — this
    // clock starts only once the bot has arrived, where a real pickup resolves
    // in a tick or two. Bots park on corpses whose loot they can never take
    // (group-roll items pending a real player's roll, items reserved for others,
    // bags full); without this they burned the full 15s yield timeout on each
    // such corpse, and the tank — which holds its advance while any follower is
    // looting — stalled the whole party for it. 3s clears a normal auto-loot
    // comfortably while cutting the dead time on a stuck corpse 5x.
    constexpr uint32 DC_LOOT_CAMP_TIMEOUT_MS = 3 * 1000;

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

    // The class opener resolved to a concrete spell the BOT has actually LEARNED.
    struct ResolvedPullSpell
    {
        uint32 spellId;
        float  minRange;
        float  maxRange;
    };

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
        ctx->GetValue<bool>("dungeon clear enabled")->Set(false);
        if (Player* bot = botAI->GetBot())
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

    // A DungeonClearApproach::Observation pre-loaded with this TU's DC_* thresholds
    // and an all-inactive state (the struct's defaults: posStuck 0, !canPursue,
    // pathReachable, !hopDone, ... -> DecideApproach returns the terminal
    // MoveToFallback). The tail phases that own a regression-prone THRESHOLD
    // decision (the posStuck tick limit, the pursuit-fail latch, the dead-end
    // escalation budget) fill in just their own state fields and consult
    // DecideApproach, so those thresholds live in one engine-free, gtested place
    // instead of inline `>=`/`<` comparisons that could drift from the spec. The
    // plain-boolean rungs (jump / glide / off-line / window / unreachable /
    // off-path) keep their flags inline — there is no threshold for the pure
    // function to own there.
    DungeonClearApproach::Observation MakeApproachObs()
    {
        DungeonClearApproach::Observation o;
        o.stuckTickLimit      = DC_STUCK_TICK_LIMIT;
        o.pursuitFailLimit    = DC_PURSUIT_FAIL_LIMIT;
        o.doneNotEngagedLimit = DC_DONE_NOT_ENGAGED_LIMIT;
        return o;
    }

    // DecideApproach + the record/replay capture hook in one call. Returns the
    // pure verdict exactly as DecideApproach would; additionally, when the run
    // has RecordDecisions on (off by default, an addon-toggleable per-run flag),
    // appends this (observation -> verdict) decision to the capture file. That
    // is the whole orchestration replay harness seam: a freeze reproduced with
    // capture on becomes a JSONL fixture that t/replay_decisions.cpp pins
    // forever. Each of Execute's staged DecideApproach consults routes through
    // here so every live decision the action acts on is captured — the verdict
    // is unchanged whether recording is on or off (capture is a side effect).
    DungeonClearApproach::Verdict DecideAndMaybeRecord(
        Player* bot, DungeonClearApproach::Observation const& o)
    {
        DungeonClearApproach::Verdict const v = DungeonClearApproach::DecideApproach(o);
        if (bot && DcSettings::GetBool(bot, "RecordDecisions"))
            DungeonClearApproachIo::Record(bot->GetGUID().GetRawValue(),
                                           getMSTime(), o, v);
        return v;
    }

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
    void InstallLongPath(Player* bot, AiObjectContext* ctx, DcApproachState& appr,
                         DungeonBossInfo const& target,
                         ChunkedPathfinder::Result&& built, uint32 now, char const* how)
    {
        ChunkedPathfinder::Result& path =
            ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Get();
        path = std::move(built);
        appr.longPathTargetEntry = target.entry;
        appr.longPathTargetPos = Position(target.x, target.y, target.z);
        appr.longPathExpiresMs = now + DC_LONG_PATH_TTL_MS;

        size_t const firstSegPts = path.segments.empty() ? 0 : path.segments.front().polyline.size();
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] path rebuilt -> {} ({}): segs={} firstSegPts={} complete={}{}",
                 bot->GetName(), target.name, how,
                 path.segments.size(), firstSegPts, path.complete,
                 path.reachable ? "" : (" UNREACHABLE: " + path.failureReason));

        ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
        ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};
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
                InstallLongPath(bot, ctx, appr, target, std::move(built), now, "async");
                return;
            }
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] discarded stale async path (built for boss {} map {}, now boss {} map {})",
                      bot->GetName(), jobEntry, jobMap, target.entry, bot->GetMapId());
            // fall through to re-evaluate need and maybe resubmit
        }

        // ---- 2. Is a (re)build due? ----
        bool const targetChanged = cachedEntry != target.entry;
        bool const expired = expiresAt == 0 || now >= expiresAt;
        // The live boss relocated (pool/wandering boss) or its live position
        // just streamed in far from the static anchor the current path was
        // built toward — retarget early instead of walking a stale route the
        // full TTL. Only meaningful once a path actually exists (expiresAt!=0).
        Position const& builtToward = appr.longPathTargetPos;
        bool const moved = expiresAt != 0 &&
            builtToward.GetExactDist(Position(target.x, target.y, target.z)) >
                DC_LONG_PATH_RETARGET_DIST;
        if (!targetChanged && !expired && !moved)
            return;

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
    bool TriggerStrideRebuild(Player* bot, AiObjectContext* ctx, DcApproachState& appr)
    {
        ChunkedPathfinder::Result const& path =
            ctx->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Get();
        DungeonFollowerState& follower =
            ctx->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get();
        if (path.reachable && !path.segments.empty() &&
            DungeonPathFollower::Resnap(bot, path, follower))
            return true;

        appr.longPathExpiresMs = 0;
        ctx->GetValue<uint32>("dungeon clear current hop")->Set(0u);
        follower = DungeonFollowerState{};
        return false;
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

    // --- Submerged swim legs (Tier A) ------------------------------------
    // 3D proximity at which the swim cursor treats a point as reached.
    constexpr float DC_SWIM_POINT_REACHED = 3.0f;
    // If the bot is farther than this from the current swim point, the leg is
    // stale (teleport / knockback / leftover from a prior run) — drop it.
    constexpr float DC_SWIM_OFFLEG_MAX = 50.0f;
    // Abandon a swim leg that makes no closing progress for this long.
    constexpr uint32 DC_SWIM_STUCK_MS = 6000;

    // Begin a swim leg from the bot's current position to (bx,by,bz). Gated on
    // SwimEnable, SwimMaxRange, and water actually lying between. Stores the leg
    // in "dungeon clear swim state"; DriveActiveSwim issues the spline next tick.
    // Returns true iff a leg was started.
    bool TryBeginSwim(Player* bot, AiObjectContext* context,
                      uint32 targetEntry, float bx, float by, float bz)
    {
        if (!bot || !DcSettings::GetBool(bot, "SwimEnable"))
            return false;

        G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        G3D::Vector3 const goal(bx, by, bz);
        if ((goal - start).length() > DcSettings::GetFloat(bot, "SwimMaxRange"))
            return false;
        if (!SwimPathfinder::WaterBetween(bot, start, goal))
            return false;

        SwimPathfinder::Result res = SwimPathfinder::Build(bot, start, goal);
        if (!res.ok || res.points.empty())
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] swim build failed: {}", bot->GetName(), res.failureReason);
            return false;
        }

        DungeonClearSwimState& swim =
            context->GetValue<DungeonClearSwimState&>("dungeon clear swim state")->Get();
        swim.Reset();
        swim.active = true;
        swim.points = std::move(res.points);
        swim.cursor = 0;
        swim.targetEntry = targetEntry;
        swim.buildStart = start;
        swim.lastProgressMs = getMSTime();
        swim.lastDistToPoint = (swim.points.front() - start).length();

        DcMovement::ResolveEscortConflict(bot);  // drop any stale navmesh glide before swimming
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] swim leg started: {} pts toward ({:.1f},{:.1f},{:.1f})",
                 bot->GetName(), swim.points.size(), bx, by, bz);
        return true;
    }

    // Drive an in-progress swim leg. Returns true if a leg is active and owned
    // the tick (caller must return true); false if no leg is active or the leg
    // just completed (caller falls through to normal navmesh navigation).
    bool DriveActiveSwim(Player* bot, PlayerbotAI* botAI, AiObjectContext* context,
                         DcApproachState& appr,
                         uint32 targetEntry, float bx, float by, float bz,
                         float engageDist, float engageRange)
    {
        DungeonClearSwimState& swim =
            context->GetValue<DungeonClearSwimState&>("dungeon clear swim state")->Get();
        if (!swim.active)
            return false;

        // Target changed since the leg was built — invalidate.
        if (swim.targetEntry != targetEntry)
        {
            swim.Reset();
            return false;
        }

        // Arrived at the boss area — hand back to the engage/ladder logic.
        if (engageDist <= engageRange)
        {
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] swim leg complete (within engage range)", bot->GetName());
            swim.Reset();
            DcMovement::ResolveEscortConflict(bot);
            return false;
        }

        G3D::Vector3 const botPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

        // Advance the cursor past points already reached (3D proximity).
        while (swim.cursor < swim.points.size() &&
               (botPos - swim.points[swim.cursor]).length() <= DC_SWIM_POINT_REACHED)
            ++swim.cursor;

        // Consumed the whole leg but still short of engage range — hand back to
        // the navmesh planner from here (the far mesh island may now reach the
        // boss; if not, the dead-end logic re-evaluates and may re-swim).
        if (swim.cursor >= swim.points.size())
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] swim leg consumed -> handing back to navmesh", bot->GetName());
            swim.Reset();
            DcMovement::ResolveEscortConflict(bot);
            appr.longPathExpiresMs = 0;
            return false;
        }

        float const distToPoint = (botPos - swim.points[swim.cursor]).length();

        // Off-leg: bot is implausibly far from the current point (teleport,
        // knockback, stale leg) — drop it and let navigation rebuild.
        if (distToPoint > DC_SWIM_OFFLEG_MAX)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] swim leg abandoned: {:.0f}yd off the leg", bot->GetName(), distToPoint);
            swim.Reset();
            DcMovement::ResolveEscortConflict(bot);
            appr.longPathExpiresMs = 0;
            return false;
        }

        // Progress watchdog. posStuck can't see a non-moving bot, so track the
        // closing distance to the current point ourselves.
        uint32 const now = getMSTime();
        if (distToPoint < swim.lastDistToPoint - 0.5f)
        {
            swim.lastDistToPoint = distToPoint;
            swim.lastProgressMs = now;
        }
        else if (getMSTimeDiff(swim.lastProgressMs, now) > DC_SWIM_STUCK_MS)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] swim leg wedged (no progress {}ms) -> abandoning",
                     bot->GetName(), getMSTimeDiff(swim.lastProgressMs, now));
            swim.Reset();
            DcMovement::ResolveEscortConflict(bot);
            StallDungeonClear(botAI,
                "Tried to swim across but got stuck underwater. Use 'dc skip' to move on.");
            return true;
        }

        // Leave a healthy in-flight escort glide alone (same re-issue discipline
        // as the long-path drive — keying on splineRunning, not the LastMovement
        // wait, so the next window chains seamlessly when the spline finalizes).
        MotionMaster* mm = bot->GetMotionMaster();
        bool const splineRunning =
            mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE && bot->isMoving();
        if (splineRunning)
        {
            SetPhase(context, "swimming");
            ClearStall(context);
            return true;
        }

        if (!mm)
            return false;

        // Build the spline window from the cursor: [live pos, remaining swim
        // points...] with SUBMERGED Z used verbatim (no UpdateAllowedPositionZ).
        Movement::PointsArray points;
        points.push_back(botPos);
        for (size_t i = swim.cursor;
             i < swim.points.size() && points.size() < DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS;
             ++i)
            points.push_back(swim.points[i]);

        // SplinePath handles stand-up / cast-interrupt / MoveSplinePath and the
        // NORMAL-priority LastMovement record (and refuses a <2-point window).
        if (!DcMovement::SplinePath(botAI, points))
        {
            swim.Reset();
            return false;
        }
        SetPhase(context, "swimming");
        ClearStall(context);
        return true;
    }

    // Drop a breadcrumb of the tank's current position onto the trail the advanced
    // pull walks back to place its camp (see DungeonClearBreadcrumbsValue). Called
    // each forward-advance tick; samples only every kSpacing yards of real
    // movement, and RESTARTS the trail on a kJump-sized gap (a pull drag-back or a
    // teleport) so the stored trail is always spatially contiguous behind the
    // tank — independent of the long-path follower cursor, which the drag resets.
    void RecordBreadcrumb(AiObjectContext* ctx, Player* bot)
    {
        if (!ctx || !bot)
            return;
        constexpr float kSpacing = 4.0f;   // min real movement between samples
        constexpr float kJump = 12.0f;     // gap that means a drag/teleport -> reset
        constexpr size_t kMax = 128;       // history cap (~ kMax*kSpacing yd)
        std::vector<Position>& crumbs =
            ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get().breadcrumbs;
        Position const cur(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (crumbs.empty())
        {
            crumbs.push_back(cur);
            return;
        }
        float const d = crumbs.back().GetExactDist2d(&cur);
        if (d < kSpacing)
            return;
        // Discontinuity guard is 3D: a drop-down / ledge can move the tank only a
        // few yards in plan view but a long way vertically. A 2D-only guard treats
        // that as contiguous trail, so a camp later picked across the seam sits on
        // a different floor and the move to it straight-lines through the geometry
        // (the "under the map" symptom). 3D distance catches the vertical jump and
        // restarts the trail so consecutive crumbs are always a straight walk apart.
        if (crumbs.back().GetExactDist(&cur) > kJump)
        {
            // Discontinuity (a drag-back in combat, a drop-down). Wiping the whole
            // trail here starves the next pull's ComputeSafeCamp exactly when it
            // matters most — and the information is almost always still valid: the
            // camp itself sits on previously walked trail. So try to REJOIN: find
            // the latest crumb near where the bot stands now and truncate forward
            // of it, keeping the contiguous prefix. Only a true teleport (no crumb
            // within kRejoinRadius) restarts the trail. kRejoinRadius (6yd) sits
            // under kJump and above DC_PULL_CAMP_ARRIVE (5yd), so a tank standing
            // at camp rejoins the crumb the camp was lifted from. Contiguity holds:
            // the prefix was already pairwise-contiguous and cur is within
            // kRejoinRadius < kJump of the rejoin crumb.
            constexpr float kRejoinRadius = 6.0f;
            std::size_t const j =
                DungeonClearMath::FindTrailRejoin(crumbs, cur, kRejoinRadius);
            if (j != DungeonClearMath::TrailRejoinNone)
                crumbs.resize(j + 1);  // rejoin — drop everything ahead of crumb j
            else
                crumbs.clear();        // true teleport — restart the trail
            crumbs.push_back(cur);
            return;
        }
        crumbs.push_back(cur);
        if (crumbs.size() > kMax)
            crumbs.erase(crumbs.begin());
    }
}

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

    // Only skirt while a room-aggro clear is genuinely active (flagged boss, room
    // trash remaining, tank at the boss room). Outside that, ordinary corridor
    // engages keep their direct line. The chord/sphere test below would also
    // no-op far from the boss, but the gate keeps the live-boss lookup off the
    // hot path for every normal walk-in.
    if (!DcTargeting::IsRoomClearActive(bot, context))
        return std::nullopt;

    std::optional<DungeonBossInfo> next =
        AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
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
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryEngageHold(AdvanceState const& st)
{
    DungeonBossInfo const* next = st.next;
    Creature* const liveBoss = st.liveBoss;
    float const engageDist = st.engageDist;
    bool const atBoss = st.atBoss;

    // Travel objectives have no engage handoff. Keep navigating to the anchor;
    // DungeonClearAtObjectiveTrigger (rel 30, outranks Advance) takes over once
    // the tank is inside the arrival radius. Holding here on the boss engage
    // range — which is wider than the arrival radius — would strand the tank
    // short of the objective forever (the at-boss trigger that would normally
    // release the hold is gated off for non-Boss anchors).
    if (next->kind != DungeonAnchorKind::Boss)
        return Step::Continue;

    if (atBoss)
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
                      IsBetweenPullsReady(bot, context) ? 1 : 0,
                      AI_VALUE(bool, "has available loot") ? 1 : 0,
                      AI_VALUE(bool, "can loot") ? 1 : 0);
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            ClearStall(context);
            // Parked at the boss waiting for the at-boss pull — not navigating,
            // so clear the nav phase (status reads this as "idle / holding").
            SetPhase(context, "");
            return Step::ReturnFalse;
        }
    }
    return Step::Continue;
}

// Loot yield (with commit-timeout). Step aside through the WHOLE loot lifecycle
// so the loot system can pick up a nearby corpse: "has available loot" is true
// only while a corpse is ~3-15yd away and flips FALSE at ~3yd (when "can loot"
// flips TRUE). Advance (engine relevance 15) outranks the loot actions (open
// loot is 8), so yielding on only one flag let advance win the tick at the 3yd
// boundary and fire a boss-bound spline before open-loot ran — the
// corpse<->boss oscillation. Yielding while EITHER flag is set keeps advance out
// of the way until the loot is actually picked up.
//
// We also hold while ANY follower still has a corpse to pick up, so the tank
// doesn't push to the next pull the instant its own loot is done and leave the
// party scrambling to catch up. IsAnyPartyMemberLooting reads each follower's
// own loot flags cross-context (same pattern as the party-tank lookup); the
// shared commit-timeout below bounds the total wait.
//
// The timeout stops us waiting forever on loot the party can't finish
// (group-loot rolls pending, bags full): after DC_LOOT_YIELD_TIMEOUT_MS we
// force-advance; when no one is looting any more the flags clear and the timer
// resets (so the next pull gets a fresh full window).
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryLootYield(AdvanceState const& /*st*/)
{
    // Before reading the flags, drop any loot we already gave up on from the
    // stock stack/target (see StripSkippedLoot). Running here — at advance's
    // relevance, above the loot pipeline — means stock can't re-pick a skipped
    // corpse this tick, so the flags below and the timeout's give-up stay in
    // sync and the yield doesn't re-arm on something we just abandoned.
    DcLootPolicy::StripSkippedLoot(botAI);
    // Proactively skip a corpse with nothing takeable for us (un-finishable
    // group-roll/reserved loot, or below DungeonClear.LootMinQuality) BEFORE we
    // walk to it, so it never arms the yield at all — the event-driven analogue
    // of the camp/timeout cutoffs below, which only fire after a wasted walk.
    DcLootPolicy::MaybeSkipUnworthyLoot(botAI);
    // Fast-skip a corpse we've been camped on too long (un-lootable) before it
    // can burn the full yield timeout below; followers do the same in their
    // follow-tank yield, which is what actually shortens IsAnyPartyMemberLooting.
    DcLootPolicy::MaybeGiveUpCampedLoot(botAI, DC_LOOT_CAMP_TIMEOUT_MS, DC_LOOT_GIVEUP_TTL_MS);
    uint32& lootYieldStart =
        context->GetValue<DcApproachState&>("dungeon clear approach state")->Get().lootYieldStartMs;
    bool const lootYield =
        AI_VALUE(bool, "has available loot") || AI_VALUE(bool, "can loot") ||
        DcPartyState::IsAnyPartyMemberLooting(bot);
    if (lootYield)
    {
        uint32 const now = getMSTime();
        if (lootYieldStart == 0)
            lootYieldStart = now;

        if (now - lootYieldStart >= DC_LOOT_YIELD_TIMEOUT_MS)
        {
            // Waited long enough — give up on THIS corpse so we stop re-arming
            // the yield on it (the corpse<->path ping-pong), then advance past.
            // GiveUpCurrentLoot blacklists our committed loot; StripSkippedLoot
            // next tick removes it so the flags clear. Don't reset lootYieldStart
            // here: keep it expired so we keep advancing until the flags drop.
            DcLootPolicy::GiveUpCurrentLoot(botAI, DC_LOOT_GIVEUP_TTL_MS);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] loot-yield timed out after {}ms -> giving up on corpse, advancing",
                     bot->GetName(), now - lootYieldStart);
        }
        else
        {
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] advance yielding: loot in progress ({}ms)",
                      bot->GetName(), now - lootYieldStart);
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            return Step::ReturnFalse;
        }
    }
    else
    {
        lootYieldStart = 0;  // not looting -> reset the commit timer
    }
    return Step::Continue;
}

// Between-pulls rest: yield so food/drink can run and stragglers catch up.
// The multiplier suppresses wander actions during the wait.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryBetweenPullsRest(AdvanceState const& /*st*/)
{
    if (!IsBetweenPullsReady(bot, context))
    {
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] advance yielding: party not ready / resting", bot->GetName());
        DcMovement::StopBot(bot, DcMovement::Stop::Hold);
        return Step::ReturnFalse;
    }
    return Step::Continue;
}

// If this boss has no live spawn at all (and not even a corpse), stall so the
// player can `dc skip` instead of being forced to re-enable the mode. Bosses
// that legitimately despawn after kill are handled by the
// InstanceScript::GetBossState probe in NextDungeonBossValue — they never reach
// here.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryBossNotPresentStall(AdvanceState const& st)
{
    DungeonBossInfo const* next = st.next;

    // Travel objectives are not creatures — "not in the creature store" is their
    // normal state. Arrival is owned by DungeonClearAtObjectiveTrigger (which
    // outranks Advance); never stall the approach to an objective.
    if (next->kind != DungeonAnchorKind::Boss)
        return Step::Continue;

    if (!DcTargeting::IsCreaturePresentOnMap(bot, next->entry))
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
            return Step::ReturnFalse;
        }
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] {} not in creature store but {:.0f}yd away (>{:.0f}) "
                  "-> advancing to stream its grid in",
                  bot->GetName(), next->name, distToBoss, DC_BOSS_GRID_LOADED_RANGE);
        // fall through to the normal advance below
    }
    return Step::Continue;
}

// Position-based stuck detection + recovery. Samples world position every tick
// (so lastPos stays current) and, once the bot has gone DC_STUCK_TICK_LIMIT
// consecutive ticks without real displacement while supposedly moving, halts the
// wedged glide and escalates Resnap -> rebuild -> navmesh-nudge -> stall.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryPosStuckRecovery(AdvanceState& st)
{
    DungeonBossInfo const* next = st.next;
    DcApproachState& appr = *st.appr;
    uint32& posStuck = appr.posStuckTicks;
    uint32& rebuildAttempts = appr.rebuildAttempts;
    Position& lastPos = appr.lastPos;

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

    // Threshold decision (posStuck >= DC_STUCK_TICK_LIMIT) sourced from the pure,
    // gtested DecideApproach: the only active field is posStuckTicks, so the
    // verdict is StuckRecover exactly when the bot has gone the tick limit without
    // displacement, else the neutral terminal.
    DungeonClearApproach::Observation stuckObs = MakeApproachObs();
    stuckObs.posStuckTicks = posStuck;
    if (DecideAndMaybeRecord(bot, stuckObs) == DungeonClearApproach::Verdict::StuckRecover)
    {
        posStuck = 0;
        // Wedged and replanning — surface "recovering" to the status poll.
        SetPhase(context, "recovering");

        // The bot was moving but not progressing — a continuous-spline glide
        // wedged against geometry. Halt it so the recovery below re-issues
        // movement from a standstill instead of fighting the stuck spline.
        DcMovement::ResolveEscortConflict(bot);

        // First-line recovery: try a Resnap onto the existing polyline
        // (cheap; handles the "knocked sideways but path is still good"
        // case). On failure, invalidate the long-path cache and reset
        // the follower so the next tick rebuilds from the bot's current
        // position. Strides are short enough that a rebuild from here
        // usually picks a different sequence of stride endpoints and
        // routes around whatever was wedging us.
        bool const resnapped = TriggerStrideRebuild(bot, context, appr);
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
            return Step::ReturnFalse;
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
                DcStatusPublisher::SendAddonMessage(botAI, "CHAT\tRepathing around " + next->name + " \xe2\x80\x94 nudging onto the navmesh.");
                return Step::ReturnTrue;
            }
            StallDungeonClear(botAI,
                "Stuck near " + next->name + " — not making forward progress. "
                "I'll try to clear nearby mobs; use 'dc skip' if it persists.");
            return Step::ReturnFalse;
        }
        return Step::ReturnFalse;
    }
    return Step::Continue;
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
// pursuitFailTicks doubles as a latch: while it sits at DC_PURSUIT_FAIL_LIMIT
// we've given up on the direct-pursuit shortcut for this approach and let the
// long-path drive uninterrupted (re-entering the pursuit branch would re-kill
// the long-path's escort glide every tick via DcMoveTo's conflict teardown
// before it travels — a step/freeze thrash). The latch clears on boss change
// and once we make it inside engage range (see the resets above).
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryDirectPursuit(AdvanceState& st)
{
    DungeonBossInfo const* next = st.next;
    Creature* const liveBoss = st.liveBoss;
    float const bossX = st.bossX, bossY = st.bossY, bossZ = st.bossZ;
    float const engageDist = st.engageDist;
    DcApproachState& appr = *st.appr;
    uint32& pursuitFailTicks = appr.pursuitFailTicks;
    uint32& stuck = appr.stuckCount;

    bool const canPursue =
        liveBoss && engageDist <= DC_DIRECT_PURSUIT_RANGE && bot->IsWithinLOSInMap(liveBoss);
    if (!canPursue)
    {
        // Boss not pursuable this tick (out of LOS / range / not loaded): clear
        // the grace counter so a later pursuit starts with a fresh budget.
        pursuitFailTicks = 0;
        return Step::Continue;
    }

    // The pursuit-fail latch boundary (pursuitFailTicks < DC_PURSUIT_FAIL_LIMIT)
    // is the regression-prone threshold here; source it from the gtested
    // DecideApproach. canPursue is true in this branch, so the verdict is Pursue
    // exactly while the latch is still open, and the long-path takes over once it
    // trips.
    DungeonClearApproach::Observation pursueObs = MakeApproachObs();
    pursueObs.canPursue = true;
    pursueObs.pursuitFailTicks = pursuitFailTicks;
    if (DecideAndMaybeRecord(bot, pursueObs) == DungeonClearApproach::Verdict::Pursue)
    {
        // DcMoveTo drops any stale long-path escort glide (so it doesn't keep
        // driving the bot toward the spawn anchor) before steering at the live boss.
        bool const chasing = DcMoveTo(next->mapId, bossX, bossY, bossZ,
                                    /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                    /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);

        // A move was issued, is already in flight, or was just queued (MoveTo's
        // own IsDuplicateMove / IsWaitingForLastMove return false while a prior
        // move is still gliding) — pursuit is alive, let it ride. A move that is
        // in flight but wedging in place is caught by the posStuck rebuild above,
        // which runs before this branch.
        if (chasing || bot->isMoving() ||
            IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        {
            pursuitFailTicks = 0;
            stuck = 0;
            ClearStall(context);
            SetPhase(context, "pursuing");
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] pursuing live {} at {:.0f}yd (LOS) -> MoveTo {}",
                      bot->GetName(), next->name, engageDist,
                      chasing ? "issued" : "in flight");
            return Step::ReturnTrue;
        }

        // MoveTo produced nothing and the bot is standing still: PathGenerator
        // can't reach the boss's live poly (Z -> INVALID_HEIGHT, or a winding
        // route past its 74-hop cap). posStuck can't see this — the bot never
        // moves — so without an escape this is a silent freeze just outside pull
        // range. Count the dead tick; past a short grace, leave the latch set and
        // fall through to the wall-screened long-path below (LongRangePathfinder,
        // no hop cap, with its own dead-end -> stall escalation).
        ++pursuitFailTicks;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] direct pursuit of {} stalled ({:.0f}yd, MoveTo noop, not "
                 "moving) {}/{}",
                 bot->GetName(), next->name, engageDist, pursuitFailTicks,
                 DC_PURSUIT_FAIL_LIMIT);
        if (pursuitFailTicks < DC_PURSUIT_FAIL_LIMIT)
            return Step::ReturnFalse;

        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] direct pursuit of {} unreachable -> long-path fallback "
                 "(latched until engage range / boss change)",
                 bot->GetName(), next->name);
        // fall through to the long-path machinery below; the latch keeps us out
        // of this branch on subsequent ticks so the long-path can travel.
    }
    // Verdict not Pursue: the latch has tripped — skip the pursuit shortcut and
    // let the long-path below drive.
    return Step::Continue;
}

// No navmesh route to the boss. Distinguishes an EXPECTED empty path (async
// build still in flight — hold quietly) from a genuine failure, attempts an
// off-mesh nudge and a swim, then stalls for the stalled-fallback / `dc skip`.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryLongPathUnreachable(AdvanceState& st)
{
    DungeonBossInfo const* next = st.next;
    float const bossX = st.bossX, bossY = st.bossY, bossZ = st.bossZ;
    DcApproachState& appr = *st.appr;
    ChunkedPathfinder::Result const& path = *st.path;

    if (!path.reachable)
    {
        // Async pathfinding (DungeonClear.AsyncPathfinding): a build is still in
        // flight — almost always right after a boss change, where EnsureLongPath
        // cleared the cache and handed the heavy A* to the worker. The empty
        // path is EXPECTED here, not a routing failure, so hold position quietly
        // and wait (the result lands within a tick or a few) instead of crying
        // "no navigable route" to the party. Mirrors the between-pulls rest
        // yield: no stall reason set, so the stalled-fallback never fires; the
        // multiplier suppresses wander while we wait.
        if (appr.pendingPathJob != 0)
        {
            SetPhase(context, "planning route");
            DcMovement::StopBot(bot, DcMovement::Stop::Soft);
            return Step::ReturnFalse;
        }

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
                SetPhase(context, "recovering");
                appr.longPathExpiresMs = 0;
                return Step::ReturnTrue;
            }
        }

        // No navmesh route at all. Before stalling, try a swim: the target may
        // sit behind a submerged tunnel the navmesh can't span (only a surface
        // sheet exists over deep water). Gated on water lying between, so a
        // genuinely land-locked failure still falls through to the stall.
        if (TryBeginSwim(bot, context, next->entry, bossX, bossY, bossZ))
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] no navmesh route to {} -> swimming", bot->GetName(), next->name);
            SetPhase(context, "swimming");
            return Step::ReturnTrue;
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
        return Step::ReturnFalse;
    }
    return Step::Continue;
}

// On-path tracking: if the bot has drifted off the planned corridor (knockback,
// charge, sticky-trash detour, follower bump) past the tick budget, Resnap onto
// the existing polyline; Resnap failure forces a rebuild and yields the tick.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryOffPathResnap(AdvanceState& st)
{
    DcApproachState& appr = *st.appr;
    ChunkedPathfinder::Result const& path = *st.path;
    DungeonFollowerState& follower = *st.follower;

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
            SetPhase(context, "recovering");
            DcMovement::ResolveEscortConflict(bot);
            appr.longPathExpiresMs = 0;
            follower = DungeonFollowerState{};
            return Step::ReturnFalse;
        }
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] off-path {} ticks -> Resnapped to seg {} pt {}",
                  bot->GetName(), offTicks, follower.segmentIdx, follower.pointIdx);
    }
    return Step::Continue;
}

// Post-combat re-anchor. NextHop only fast-forwards the cursor past points the
// tank passed within POINT_REACHED; a trash chase displaces it well off those
// points — often FORWARD along the route — leaving the cursor stale and behind,
// so the tank would walk backward to it. If the next hop is implausibly far for
// normal gliding, re-anchor onto the nearest visible route point (Resnap is
// LOS-gated, so it won't snap across a wall) and re-fetch the hop. This never
// terminates the tick; it only mutates the cursor/hop for the phases below.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryReanchorStaleCursor(AdvanceState& st)
{
    ChunkedPathfinder::Result const& path = *st.path;
    DungeonFollowerState& follower = *st.follower;
    DungeonPathFollower::Hop& hop = st.hop;

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
    return Step::Continue;
}

// The long-path completed (cursor reached the polyline end) while still outside
// engage range: a benign rebuild when already in range, else the "route
// dead-ends short of the boss" wedge — a few straight-line final-approach
// MoveTo attempts, then a swim, then a stall for `dc skip`.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryHopDoneEscalation(AdvanceState& st)
{
    DungeonBossInfo const* next = st.next;
    float const bossX = st.bossX, bossY = st.bossY, bossZ = st.bossZ;
    float const engageDist = st.engageDist, engageRange = st.engageRange;
    DcApproachState& appr = *st.appr;
    DungeonFollowerState& follower = *st.follower;
    uint32& doneNotEngagedTicks = appr.doneNotEngagedTicks;

    if (!st.hop.isDone)
        return Step::Continue;

    // Cursor reached the polyline end. If we're already within engage range
    // this is a benign "anchored hops were still pending at the top" case —
    // rebuild and let the engage hold take over next tick.
    if (engageDist < engageRange)
    {
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] reached end of path polyline (seg {}) -> forcing rebuild next tick",
                 bot->GetName(), follower.segmentIdx);
        appr.longPathExpiresMs = 0;
        return Step::ReturnFalse;
    }

    // The route dead-ends short of the boss. Rebuilding here just produces
    // the same 0-point path next tick (we're sitting on its terminal poly),
    // and since the bot isn't moving the posStuck counter never escalates —
    // this is the silent forever-loop. Try a straight final-approach MoveTo
    // first: PathGenerator may close a few yards the chunk builder gave up
    // on, or the boss may have wandered into reach. Past the retry budget,
    // declare it unreachable and stall for `dc skip`.
    ++doneNotEngagedTicks;
    // Dead-end escalation budget (doneNotEngagedTicks < DC_DONE_NOT_ENGAGED_LIMIT)
    // sourced from the gtested DecideApproach. We are past the engageDist <
    // engageRange case above, so with hopDone the verdict is FinalApproach exactly
    // while the retry budget holds, then escalates (swim-then-stall) once spent.
    DungeonClearApproach::Observation hopObs = MakeApproachObs();
    hopObs.hopDone = true;
    hopObs.engageDist = engageDist;
    hopObs.engageRange = engageRange;
    hopObs.doneNotEngagedTicks = doneNotEngagedTicks;
    if (DecideAndMaybeRecord(bot, hopObs) == DungeonClearApproach::Verdict::FinalApproach)
    {
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] path ends {:.0f}yd short of {} (>{:.0f}, attempt {}/{}) "
                 "-> final-approach MoveTo",
                 bot->GetName(), engageDist, next->name, engageRange,
                 doneNotEngagedTicks, DC_DONE_NOT_ENGAGED_LIMIT);
        bool const pushing = DcMoveTo(next->mapId, bossX, bossY, bossZ,
                                    /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                    /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);
        SetPhase(context, "pursuing");
        appr.longPathExpiresMs = 0;
        return pushing ? Step::ReturnTrue : Step::ReturnFalse;
    }

    doneNotEngagedTicks = 0;

    // Last resort before stalling: if the dead-end is a water gate (a
    // submerged tunnel the surface-sheet navmesh can't descend into), swim
    // the rest of the way in 3D. Gated on water lying between, so a real
    // ledge / gap dead-end still stalls as before.
    if (TryBeginSwim(bot, context, next->entry, bossX, bossY, bossZ))
    {
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] route dead-ends short of {} -> swimming the rest",
                 bot->GetName(), next->name);
        SetPhase(context, "swimming");
        return Step::ReturnTrue;
    }

    LOG_INFO("playerbots.dungeonclear",
             "[DC:{}] {} unreachable: route dead-ends {:.0f}yd short after {} approach "
             "attempts -> stalling",
             bot->GetName(), next->name, engageDist, DC_DONE_NOT_ENGAGED_LIMIT);
    StallDungeonClear(botAI,
        "Can't reach " + next->name + ": the route dead-ends short of it "
        "(likely on a ledge or across a gap the navmesh doesn't span). "
        "Use 'dc skip' to move to the next boss.");
    return Step::ReturnFalse;
}

// Anchor-declared jumps: use JumpTo (MotionMaster::MoveJump) instead of MoveTo.
// Required for dungeon drop-downs the mmap doesn't model (OK upper->lower,
// Pinnacle Skadi catwalk, AN spider tunnels, etc.).
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryJumpLeg(AdvanceState& st)
{
    DungeonBossInfo const* next = st.next;
    DungeonPathFollower::Hop const& hop = st.hop;

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
            return Step::ReturnFalse;
        }
        ClearStall(context);
        SetPhase(context, "moving");
        return Step::ReturnTrue;
    }
    return Step::Continue;
}

// A healthy in-flight continuous-spline glide just rides: NextHop already
// advanced the cursor past the glided-over points, so re-issuing would
// StopMoving + Launch a fresh escort and hitch. Keyed on splineRunning alone
// (ESCORT generator active AND moving) — deliberately NOT IsWaitingForLastMove,
// whose window-sized delay was the mid-path "frozen for seconds" freeze.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryRideLiveGlide(AdvanceState& st)
{
    DcApproachState& appr = *st.appr;
    MotionMaster* mm = bot->GetMotionMaster();
    bool const splineRunning =
        mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE && bot->isMoving();
    if (splineRunning)
    {
        appr.stuckCount = 0;
        ClearStall(context);
        SetPhase(context, "moving");
        return Step::ReturnTrue;
    }
    return Step::Continue;
}

// Re-entry leg must be a GENERATED path. After a trash chase the tank ends well
// off the planned line; the Resnap above re-anchored the cursor to the nearest
// VISIBLE forward route point, but the bot is still physically off the corridor.
// The escort spline's opening leg is a STRAIGHT segment to that point — BotCanSee
// only cleared a thin eye-ray, so the floor-walking straight line still cuts
// across wall corners / the inside of a bend (the "snaps back through the wall
// after combat" report). While off the line, rejoin with a PathGenerator-built
// route; the continuous glide resumes once RouteDeviation drops back under the
// on-corridor threshold.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryOffLineRejoin(AdvanceState& st)
{
    DungeonBossInfo const* next = st.next;
    DcApproachState& appr = *st.appr;
    ChunkedPathfinder::Result const& path = *st.path;
    DungeonFollowerState& follower = *st.follower;
    DungeonPathFollower::Hop const& hop = st.hop;

    float const deviation = DungeonPathFollower::RouteDeviation(bot, path, follower);
    if (deviation > DungeonPathFollower::OFF_PATH_THRESHOLD)
    {
        // DcMoveTo cancels any stale straight spline so it can't shadow the pathed re-entry.
        bool const rejoining =
            DcMoveTo(next->mapId, hop.point.x, hop.point.y, hop.point.z,
                     /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                     /*exact_waypoint*/ false, MovementPriority::MOVEMENT_NORMAL);
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] off-line {:.1f}yd -> rejoining route via generated path to "
                  "({:.1f},{:.1f},{:.1f}) (seg {} pt {}, moved={})",
                  bot->GetName(), deviation, hop.point.x, hop.point.y, hop.point.z,
                  follower.segmentIdx, follower.pointIdx, rejoining);
        appr.stuckCount = 0;
        ClearStall(context);
        SetPhase(context, "moving");
        // Own the tick whether or not MoveTo issued: a false return is the benign
        // duplicate / waiting-on-last-move case (the pathed re-entry is already in
        // flight), and we must never fall through to launch the straight escort
        // spline while the bot is still off the line.
        return Step::ReturnTrue;
    }
    return Step::Continue;
}

// Normal case: hand the whole upcoming polyline run to the core as ONE
// EscortMovementGenerator spline so the bot glides continuously instead of
// stopping dead at every ~8yd polyline point and idling until the next tick
// (the "step, pause 2-3s, step" stutter). The escort generator builds a LINEAR
// spline, preserving the LOS-screened polyline's wall-safety without the stops.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TrySplineWindowIssue(AdvanceState& st)
{
    DcApproachState& appr = *st.appr;
    ChunkedPathfinder::Result const& path = *st.path;
    DungeonFollowerState& follower = *st.follower;

    // Build the spline window from the current cursor. It stops before any
    // jump leg (handled by the JumpTo branch above on a later tick) and at
    // MAX_SPLINE_WINDOW_POINTS. window[0] is the live position; [1..] are
    // the polyline points to glide through. SplinePath handles the stand-up /
    // cast-interrupt / MoveSplinePath ritual and the NORMAL-priority LastMovement
    // record (sized to the window travel time, for priority arbitration only),
    // and refuses a <2-point window.
    std::vector<G3D::Vector3> const window =
        DungeonPathFollower::BuildSplineWindow(bot, path, follower);
    Movement::PointsArray points(window.begin(), window.end());
    if (DcMovement::SplinePath(botAI, points))
    {
        appr.stuckCount = 0;
        ClearStall(context);
        SetPhase(context, "moving");
        return Step::ReturnTrue;
    }
    return Step::Continue;
}

// Terminal phase: the next leg is a jump or a lone anchor tail (window < 2
// points), so spline issuance isn't possible. Issue the single short hop —
// short enough that the engine's per-MoveTo re-pathfind never trips
// PATHFIND_SHORT, the same wall-safety the spline path preserves. Always handles
// the tick (the bottom of the ladder); only escalates to a stall after several
// consecutive MoveTo no-ops.
DungeonClearAdvanceAction::Step DungeonClearAdvanceAction::TryMoveToFallback(AdvanceState& st)
{
    DungeonBossInfo const* next = st.next;
    DcApproachState& appr = *st.appr;
    DungeonFollowerState& follower = *st.follower;
    DungeonPathFollower::Hop const& hop = st.hop;
    uint32& stuck = appr.stuckCount;

    LOG_DEBUG("playerbots.dungeonclear",
              "[DC:{}] spline window <2 pts -> per-point MoveTo fallback to "
              "({:.1f},{:.1f},{:.1f}) (seg {} pt {})",
              bot->GetName(), hop.point.x, hop.point.y, hop.point.z,
              follower.segmentIdx, follower.pointIdx);
    bool const moved = DcMoveTo(next->mapId, hop.point.x, hop.point.y, hop.point.z,
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
            appr.longPathExpiresMs = 0;
            follower = DungeonFollowerState{};
            StallDungeonClear(botAI,
                "Stuck near " + next->name + " — I have a path but movement isn't progressing. "
                "I'll try to clear nearby mobs; use 'dc skip' if it persists.");
            return Step::ReturnFalse;
        }
        return Step::ReturnFalse;
    }

    stuck = 0;
    ClearStall(context);
    SetPhase(context, "moving");
    return Step::ReturnTrue;
}

bool DungeonClearAdvanceAction::Execute(Event /*event*/)
{
    // Hard pause guard. The engine builds its action queue from the triggers
    // that fired at the START of the tick; on the tick the door-blocked action
    // auto-pauses, `advance` was already queued (paused was still false then) and
    // would otherwise execute right after door-blocked sets the flag — issuing a
    // fresh long escort glide that carries the tank straight through the door it
    // just parked at. The trigger's IsEnabled gate can't catch an already-queued
    // action, so re-check here and bail before issuing any movement. (Confirmed
    // from a capture: PARK -> auto-pausing -> "advance tick" -> "spline issued".)
    if (AI_VALUE(bool, "dungeon clear paused"))
        return false;

    // Lay down the breadcrumb trail the advanced pull places its camp from. Only
    // while out of combat (forward route progress) so the trail stays the cleared
    // path, not a combat-chase scribble.
    if (!bot->IsInCombat())
        RecordBreadcrumb(context, bot);

    // In pull mode the party holds at a camp and leapfrogs camp-to-camp while the
    // tank scouts ahead alone. Make sure a camp always exists for them to hold at:
    // seed it at our current spot whenever it's unset (pull mode just toggled on,
    // or a reset cleared it). Real pulls overwrite it with the computed safe camp.
    if (AI_VALUE(bool, "dungeon clear pull mode current"))
    {
        Position& camp = context->GetValue<DcPullContext&>("dungeon clear pull context")->Get().camp;
        bool const campUnset =
            camp.GetPositionX() == 0.0f && camp.GetPositionY() == 0.0f &&
            camp.GetPositionZ() == 0.0f;
        if (campUnset)
        {
            // Seed from the trail (setback behind the tank along walked ground)
            // rather than at the tank's feet, for the same monotone-party-motion
            // reason as the dynamic-upgrade seed in UpdateDynamicPullMode: a
            // feet-seed has the party walk forward TO the tank instead of holding
            // behind it. ComputeTrailCamp falls back to the tank position itself
            // when no trail exists yet (mode just toggled on, tank hasn't moved).
            float const setback = DcSettings::GetFloat(bot, "PullSetback");
            float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
            std::optional<Position> const seed =
                DcPullPlanner::ComputeTrailCamp(botAI, setback, maxDrag);
            camp = seed ? *seed
                        : Position(bot->GetPositionX(), bot->GetPositionY(),
                                   bot->GetPositionZ());
        }

        // TRAIL the camp forward while merely scouting (phase Idle, out of combat).
        // Without this the camp stays frozen at the LAST fight's spot until a new
        // pull commits, so after every camp fight the tank glides ahead to the next
        // pack while the party runs all the way BACK to the stale camp — the huge
        // tank/party gap the player reported. By creeping the camp to a point
        // PullSetback behind the moving tank each tick, hold-at-camp re-issues the
        // followers toward it so they walk ALONG behind the tank and pause at its
        // trailing position, exactly as a real party would.
        //
        // Ownership is by TIMESTAMP, not by "is a pack in pull-scan range": the
        // pull action stamps campPublishedMs on every camp write, and this trail
        // defers only while that stamp is fresh (DC_CAMP_PUBLISH_FRESH_MS). The
        // old GetPullTarget probe was a weaker condition than the gates the pull
        // TRIGGER actually needs to fire (no tank loot, abort-target pack, party
        // ready) — any tick the two disagreed NOBODY moved the camp, and with the
        // spread gate anchored at that frozen camp (right where the party stood,
        // post-fight) the tank kept gliding away unchecked: the scout-runaway gap.
        // Forward-only: adopt the new trailing point only when it sits closer to
        // the tank than the current camp (i.e. more forward), with a few yards of
        // hysteresis, so tick jitter never churns it or drags the party backward.
        DcPullContext const& pullForTrail =
            context->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
        bool const pullOwnsCamp =
            getMSTimeDiff(pullForTrail.campPublishedMs, getMSTime()) < DC_CAMP_PUBLISH_FRESH_MS;
        if (!bot->IsInCombat() &&
            pullForTrail.phase == DcPullPhase::Idle && !pullOwnsCamp)
        {
            float const setback = DcSettings::GetFloat(bot, "PullSetback");
            float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
            if (std::optional<Position> trail =
                    DcPullPlanner::ComputeTrailCamp(botAI, setback, maxDrag))
            {
                Position const tankPos(bot->GetPositionX(), bot->GetPositionY(),
                                       bot->GetPositionZ());
                if (campUnset ||
                    trail->GetExactDist2d(&tankPos) + 3.0f < camp.GetExactDist2d(&tankPos))
                {
                    camp = *trail;
                    DC_PULL_TRACE("[DC:{}] scout: trailing camp -> ({:.1f},{:.1f},{:.1f}) "
                                  "{:.1f}yd behind tank", bot->GetName(),
                                  camp.GetPositionX(), camp.GetPositionY(),
                                  camp.GetPositionZ(), tankPos.GetExactDist2d(&camp));
                }
            }
        }
    }

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

    // All per-approach counters/latches + the long-path cache state live in one
    // owned struct (see DcApproachState); the local references below alias its
    // fields so the phase logic reads/writes one place and resets in lockstep.
    DcApproachState& appr =
        context->GetValue<DcApproachState&>("dungeon clear approach state")->Get();

    // Effective boss position: a wandering/patrolling boss is rarely at its
    // static DB spawn coords, so prefer its LIVE creature position whenever it
    // is loaded on the map. Engage-range gating, the at-boss handoff, and the
    // final-approach pursuit below all key off this, so the tank chases where
    // the boss actually is instead of parking at the spawn anchor. Falls back to
    // the static coords when the creature isn't loaded (far grid not streamed in
    // yet — see DC_BOSS_GRID_LOADED_RANGE).
    Creature* const liveBoss = DcTargeting::GetLiveBoss(bot, context, next->entry);
    float const bossX = liveBoss ? liveBoss->GetPositionX() : next->x;
    float const bossY = liveBoss ? liveBoss->GetPositionY() : next->y;
    float const bossZ = liveBoss ? liveBoss->GetPositionZ() : next->z;
    float const engageDist = bot->GetDistance(bossX, bossY, bossZ);

    // Hand-off distance: the boss's real aggro bubble (+reaches/margin) when it
    // is loaded, else the static fallback. Shrinking this for a small-aggro boss
    // lets the smooth long-path/direct-pursuit glide carry the tank most of the
    // way in before the engage pull takes over — collapsing the stutter-creep
    // the old fixed 22yd hand-off produced. Must match the trigger ladder's
    // BossEngageRange so action and triggers agree on "are we at the boss".
    float const engageRange =
        DcEngageGeometry::BossEngageRange(bot, context, *next, DC_ENGAGE_RANGE);

    // "At the boss" for the route->engage handoff: close enough AND on the
    // boss's own floor. Distinct from engageDist < engageRange (pure 3D), which
    // is true while the tank passes UNDER an upper-floor boss en route to the
    // ramp — honoring it there stops the tank dead under the boss forever. Must
    // match the trigger ladder, which gates on the same predicate.
    bool const atBoss =
        DcTickMemoAccess::AtBossEngage(bot, context, *next);

    // Back inside engage range: clear the dead-end escalation counter and the
    // direct-pursuit give-up latch so a boss that wanders back out can be
    // re-pursued cleanly (the counters themselves live on appr and are consumed
    // by TryHopDoneEscalation / TryDirectPursuit below).
    if (engageDist < engageRange)
        appr.OnEnteredEngageRange();

    // Bundle the per-tick approach state for the extracted phase steps below.
    AdvanceState st;
    st.next = &*next;
    st.liveBoss = liveBoss;
    st.bossX = bossX;
    st.bossY = bossY;
    st.bossZ = bossZ;
    st.engageDist = engageDist;
    st.engageRange = engageRange;
    st.atBoss = atBoss;

    // An active submerged swim leg owns the tick outright: it drives a raw 3D
    // escort spline through a tunnel the navmesh can't model (the floor under
    // liquid is discarded at mmap-build time), so NONE of the navmesh-bound
    // logic below must run while it is active. Crucially this runs BEFORE the
    // phase ladder: mid-tunnel the boss is often unloaded, which would trip
    // TryBossNotPresentStall and abort the swim. It self-clears on arrival
    // (engage range), on consuming the leg, or on going stale, then falls
    // through to normal navigation.
    if (DriveActiveSwim(bot, botAI, context, appr, next->entry, bossX, bossY, bossZ,
                        engageDist, engageRange))
        return true;

    // Phase ladder. Each step either handles the tick (and Execute returns the
    // carried bool) or falls through to the next. The pre-route rungs come
    // first; the counter-coupled tail (stuck recovery / direct pursuit /
    // long-path drive / hop cluster) follows after the boss-change bookkeeping.
    // Loot yield runs BEFORE engage-hold. Both hold identically (StopBot(Hold)),
    // but TryLootYield also runs the loot give-up cutoffs (StripSkippedLoot /
    // MaybeSkipUnworthyLoot / MaybeGiveUpCampedLoot + the yield-timeout give-up).
    // If engage-hold ran first it would short-circuit those the moment the tank
    // reached the boss — and the at-boss TRIGGER gates on the STRICT
    // IsBetweenPullsReady (requireNoLoot), so a pending-but-unfinishable corpse
    // by the boss would block the pull forever while the give-up that clears
    // `has available loot` never got a tick: the tank parked at the boss jittering
    // (loot-walk vs hold) until the boss died by other means. Loot first lets the
    // cutoffs clear the corpse and reopen the pull.
    if (Step s = TryLootYield(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TryEngageHold(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TryBetweenPullsRest(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TryBossNotPresentStall(st); s != Step::Continue)
        return s == Step::ReturnTrue;

    // Bookkeeping: on a boss change wipe the per-approach counters so a stale
    // count from the previous pull doesn't bleed into the new approach. The
    // sticky engage-trash target isn't part of the approach struct — reset it
    // alongside the counter reset.
    if (appr.lastTargetEntry != next->entry)
    {
        appr.OnBossChange(next->entry);
        context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    }

    // Tail ladder. appr (the owned approach FSM) is shared by every rung; the
    // pre-route rungs run first, then the long-path is resolved into st, then
    // the hop-cluster rungs drive the actual movement.
    st.appr = &appr;

    if (Step s = TryPosStuckRecovery(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TryDirectPursuit(st); s != Step::Continue)
        return s == Step::ReturnTrue;

    // Build/refresh the long-path cache toward the boss's EFFECTIVE position
    // (live creature coords when loaded, else the static spawn anchor) and
    // resolve the shared path + follower the hop-cluster rungs below consume.
    DungeonBossInfo effectiveTarget = *next;
    effectiveTarget.x = bossX;
    effectiveTarget.y = bossY;
    effectiveTarget.z = bossZ;
    EnsureLongPath(bot, context, appr, effectiveTarget);
    ChunkedPathfinder::Result const& path =
        AI_VALUE(ChunkedPathfinder::Result&, "dungeon clear long path");
    DungeonFollowerState& follower =
        context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get();
    st.path = &path;
    st.follower = &follower;

    if (Step s = TryLongPathUnreachable(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TryOffPathResnap(st); s != Step::Continue)
        return s == Step::ReturnTrue;

    // One NextHop call — it advances the follower cursor, so the resulting hop
    // is carried through the hop-cluster rungs in st (never recomputed).
    st.hop = DungeonPathFollower::NextHop(bot, path, follower);
    TryReanchorStaleCursor(st);  // mutates st.hop / cursor; never terminates the tick

    // Sync the legacy "current hop" telemetry — `dc status` and a few tests
    // still read it. Map the flattened polyline cursor onto its segment index.
    context->GetValue<uint32>("dungeon clear current hop")->Set(follower.segmentIdx);

    if (Step s = TryHopDoneEscalation(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TryJumpLeg(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TryRideLiveGlide(st); s != Step::Continue)
        return s == Step::ReturnTrue;

    if (!IsMovingAllowed())
        return false;

    if (Step s = TryOffLineRejoin(st); s != Step::Continue)
        return s == Step::ReturnTrue;
    if (Step s = TrySplineWindowIssue(st); s != Step::Continue)
        return s == Step::ReturnTrue;

    // Terminal rung: always handles the tick.
    return TryMoveToFallback(st) == Step::ReturnTrue;
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
            return false;
        Unit* trash = DcTargeting::NearestRoomTrash(bot, context);
        if (!trash)
            return false;  // room clear / nothing reachable — let the boss gate open
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

    // Completed or Skipped: latch the event under its synthetic key so the
    // trigger stops re-firing it and the clear proceeds.
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
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] follow-tank: released (DC tank gone) -> cleared "
                     "follow generator (selfRealPlayer={})",
                     bot->GetName(), botAI && botAI->IsRealPlayer() ? 1 : 0);
            followedTank = ObjectGuid::Empty;
            // Cleanly torn down by us -> drop the orphan-reaper mark; there is no
            // longer a follow generator for it to chase down.
            DcFollowerLifecycle::UnmarkFollowing(bot->GetGUID());
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
    //
    // Strip already-given-up loot first (above the loot pipeline's relevance),
    // so a corpse this follower abandoned can't keep the flags below true and
    // re-arm the yield each time it drifts back within lootDistance of the
    // tank — the corpse<->tank ping-pong.
    DcLootPolicy::StripSkippedLoot(botAI);
    // Proactively skip a corpse with nothing takeable for this follower (un-
    // finishable group-roll/reserved loot, or below DungeonClear.LootMinQuality)
    // BEFORE it walks over — so it never steps off follow for it and never adds
    // to the tank's IsAnyPartyMemberLooting wait. Event-driven counterpart to
    // the camp/timeout cutoffs, which only fire after the walk is wasted.
    DcLootPolicy::MaybeSkipUnworthyLoot(botAI);
    // Fast-skip a corpse this follower has been camped on too long instead of
    // waiting out the full yield timeout: an un-finishable corpse (group-roll
    // items pending, bags full) otherwise wastes 15s here AND keeps the tank's
    // IsAnyPartyMemberLooting true, stalling the whole party on it.
    DcLootPolicy::MaybeGiveUpCampedLoot(botAI, DC_LOOT_CAMP_TIMEOUT_MS, DC_LOOT_GIVEUP_TTL_MS);
    uint32& lootYieldStart =
        context->GetValue<DcApproachState&>("dungeon clear approach state")->Get().lootYieldStartMs;
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
        // Waited long enough — give up on THIS corpse (blacklist it so the
        // flags drop next tick and we stop being yanked back to it), then resume
        // following to rejoin the tank. Leave lootYieldStart expired so we keep
        // following until the flags clear (which resets the timer).
        DcLootPolicy::GiveUpCurrentLoot(botAI, DC_LOOT_GIVEUP_TTL_MS);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] follow-tank loot-yield timed out after {}ms -> giving up on corpse, following",
                 bot->GetName(), now - lootYieldStart);
    }
    else
    {
        lootYieldStart = 0;  // not looting -> reset the commit timer
    }

    // In DYNAMIC pull mode, trail the tank at a lag distance while it scouts toward
    // the next pack and decides Leeroy vs Advanced (leader out of combat, phase
    // Idle). The tight follow bubble otherwise dragged the party onto the tank's
    // heels and into the pack's aggro arc before the tank committed, accidentally
    // pulling — the reported bug. This can NOT be done through Follow()'s distance
    // arg: Follow() early-outs whenever the bot is already inside the global
    // followDistance and refuses to re-issue an existing follow on the same target,
    // so it can only ever CLOSE a gap, never widen one (the previous attempt was a
    // silent no-op). Manage the spacing directly instead: hold inside the lag
    // bubble (let the tank pull ahead), and when the tank has pulled beyond it step
    // up to the lag ring and stop rather than charging back into the tight bubble.
    // The instant the tank commits — enters combat (Leeroy) or marks a camp
    // (Advanced hands the party to hold-at-camp, so follow-tank stands down) — this
    // gate flips false and the party reverts to the tight follow / camp hold below.
    if (DcLeaderSignal::IsLeaderDynamicScouting(bot))
    {
        float const lag = DcSettings::GetFloat(bot, "PullDynamicPartyLag");
        float const toTank = bot->GetExactDist2d(tank);
        // Keep the teardown/orphan-reaper bookkeeping live across the whole window
        // so a follow generator we (or a prior tick) installed is still cancelled
        // when the DC tank goes away.
        followedTank = tank->GetGUID();
        DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
        if (toTank <= lag)
        {
            // Inside the lag bubble: hold position. Tear down any leftover
            // continuous MoveFollow so the bot actually stops instead of creeping
            // back to the tight followDistance.
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            DC_PULL_TRACE("[DC:{}] scout-lag: holding {:.1f}yd behind tank (lag {:.1f})",
                          bot->GetName(), toTank, lag);
            // Return FALSE, not true: the bot is already stopped, so yield the tick
            // to the lower-relevance pipeline (eat/drink, loot) exactly as stock
            // Follow() does on its "no need to follow" in-range early-out. Consuming
            // the tick here (return true) suppressed the out-of-combat rest routine
            // — the party held forever at the lag ring and never drank, deadlocking
            // the tank's between-pulls wait on the party's mana.
            return false;
        }
        // Tank pulled beyond the lag: step up to a point `lag` yards behind the
        // tank ALONG ITS BREADCRUMB TRAIL — the ground the tank actually walked,
        // which its escort spline already corridor-centered. The earlier version
        // projected a geometric lag point off the tank (tx + bearing*lag) and
        // MoveTo'd it through the raw PathGenerator: no corridor centering, so
        // followers cut their own wall-hugging corners, and the projected point
        // itself (bot's own Z, straight off the tank) could land on a ledge lip —
        // the reported "hugging walls / falling off ledges in dynamic pull". Trail
        // points are reachability-gated centered crumbs, so the move stays on the
        // safe route the tank already cleared.
        Position trailPoint;
        if (DcLeaderSignal::GetLeaderScoutTrailPoint(bot, lag, trailPoint))
        {
            // ARRIVAL HOLD. The hold gate above measures STRAIGHT-LINE distance to
            // the tank (toTank <= lag), but the trail point is the crumb at `lag`
            // yards of WALKED (path) distance behind the tank. On a curved corridor
            // the two disagree: a crumb 15yd back along the trail can sit ~16yd
            // straight-line from the tank, so a follower standing ON that crumb
            // still reads toTank > lag and never satisfies the hold gate. Without
            // this guard it re-issues MoveTo to the crumb it already occupies every
            // tick, micro-stepping around it forever — the reported "two steps
            // forward, two steps back" dance — and, because it's perpetually
            // "moving", never sits to drink/eat, stalling the tank's between-pulls
            // rest gate on its mana. So: if the bot has effectively reached the
            // trail point, HOLD here (same teardown + tick-yield as the in-bubble
            // branch) instead of demanding a straight-line gate it can't meet while
            // parked on a curved crumb. The slack is just the path-vs-straight
            // curvature over one lag; a few yards covers it without parking short.
            constexpr float kTrailArrival = 4.0f;
            if (bot->GetExactDist(&trailPoint) <= kTrailArrival)
            {
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
                DC_PULL_TRACE("[DC:{}] scout-lag: holding at trail point "
                              "({:.1f}yd behind tank, lag {:.1f})",
                              bot->GetName(), toTank, lag);
                return false;
            }
            // normal_only: reject (don't straight-line to) a point that isn't
            // reachable over a real navmesh path. Crumbs are already gated for
            // reachability, but keep the guard as a belt-and-braces backstop.
            if (DcMoveTo(bot->GetMapId(), trailPoint.GetPositionX(),
                       trailPoint.GetPositionY(), trailPoint.GetPositionZ(),
                       false, false, /*normal_only=*/true))
            {
                DC_PULL_DEBUG("[DC:{}] scout-lag: trailing tank along breadcrumbs "
                              "(was {:.1f}yd behind, lag {:.1f})",
                              bot->GetName(), toTank, lag);
                return true;
            }
        }
        // No trail yet (pull mode just toggled on, tank hasn't moved) or the trail
        // point was unreachable: fall through to a normal follow so the party never
        // gets permanently stranded.
    }

    // Room-aggro skirt for followers. While the leader clears a room before a boss
    // whose engage drags the WHOLE room into combat, the tank routes its OWN
    // approach AROUND the boss's aggro sphere (RoomAggroSkirtPoint). A follower
    // close-following the tank, though, makes a STRAIGHT line to it — and if the
    // tank is on the far side of the sphere (or just walked around it), that line
    // cuts through the aggro range and wakes the boss even though the tank dodged
    // it: the reported failure. So when the direct line to the tank clips the
    // sphere, walk the follower around it on the same short-arc detour the tank
    // uses (AggroSafeApproachPoint with the TANK as the move target), and revert to
    // the normal follow the instant a straight shot at the tank is clear. Skipped
    // entirely outside an active room clear (the lookup is a cheap no-op). The
    // dynamic scout-lag branch above already keeps the party on the tank's safe
    // breadcrumb trail in pull mode, so this only governs the tight close-follow.
    {
        Position bossCenter;
        float safeRadius = 0.0f;
        if (DcLeaderSignal::GetLeaderRoomAggroSphere(bot, bossCenter, safeRadius) &&
            DcEngageGeometry::NeedsRoomAggroSkirt(
                bot->GetPositionX(), bot->GetPositionY(),
                tank->GetPositionX(), tank->GetPositionY(),
                bossCenter.GetPositionX(), bossCenter.GetPositionY(), safeRadius))
        {
            if (std::optional<Position> wp = DcEngageGeometry::AggroSafeApproachPoint(
                    bot, bossCenter.GetPositionX(), bossCenter.GetPositionY(),
                    bossCenter.GetPositionZ(), safeRadius, tank))
            {
                // Keep the teardown/orphan-reaper bookkeeping live so a leftover
                // continuous MoveFollow is still cancelled when the DC tank goes
                // away; the point-move below supersedes the follow generator (same
                // as the scout-lag trail branch above — no explicit Stop needed).
                followedTank = tank->GetGUID();
                DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
                bool const moved = DcMoveTo(bot->GetMapId(), wp->GetPositionX(),
                                          wp->GetPositionY(), wp->GetPositionZ(),
                                          false, false, /*normal_only=*/false);
                if (moved || bot->isMoving())
                {
                    DC_PULL_DEBUG("[DC:{}] follow-tank: skirting room-aggro sphere "
                                  "(r={:.1f}) -> detour ({:.1f}, {:.1f})",
                                  bot->GetName(), safeRadius,
                                  wp->GetPositionX(), wp->GetPositionY());
                    return true;
                }
            }
        }
    }

    // Tighter cluster than default. Keeps followers in healer LOS and out
    // of mob aggro-radius arcs during the advance. Default followDistance
    // (~10yd) had them strung out by the time the tank engaged.
    float const dist = std::min<float>(sPlayerbotAIConfig.followDistance, 6.0f);
    // Remember who we're chasing so the teardown branch above can cancel this
    // continuous MoveFollow once the DC tank goes away.
    followedTank = tank->GetGUID();
    // Record that this player now carries a follow generator so the world-tick
    // orphan reaper can cancel it if the AI is deleted out from under us — a
    // self-bot leaving bot mode never runs the teardown branch above.
    DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
    // Explicit per-bot angle: every follower here is a self-bot (master ==
    // itself), and MovementAction::GetFollowAngle() skips the master while
    // scanning the group — a self-bot never matches its own entry and falls out
    // at 0.0f — so the default Follow() overload gave the WHOLE party the same
    // follow slot and stacked it on one point (which is also what kept the
    // stock collision shuffle permanently armed). Same deterministic
    // golden-angle fan as ComputeCampSlot, so the cluster spreads evenly and
    // each bot's slot never moves between ticks.
    uint32 const seed = static_cast<uint32>(bot->GetGUID().GetCounter());
    float const angle =
        Position::NormalizeOrientation(static_cast<float>(seed) * 2.39996323f);
    return Follow(tank, dist, angle);
}

bool DungeonClearFilterLootAction::Execute(Event /*event*/)
{
    // Same loot-floor enforcement the advance/follow-tank actions run inline
    // while active (see TryLootYield), lifted out so it keeps running while the
    // run is paused. Drop anything already given up from the stock stack/target,
    // proactively skip any in-range corpse/chest below DungeonClear.LootMinQuality
    // (or an un-finishable group-roll, or any chest while IgnoreChests is set),
    // and time out a corpse we've been camped on. All three only prune the stock
    // loot stack/target — no movement here.
    DcLootPolicy::StripSkippedLoot(botAI);
    DcLootPolicy::MaybeSkipUnworthyLoot(botAI);
    DcLootPolicy::MaybeGiveUpCampedLoot(botAI, DC_LOOT_CAMP_TIMEOUT_MS, DC_LOOT_GIVEUP_TTL_MS);
    // Return false: we only removed loot the bot must NOT take. Returning false
    // lets the engine fall through to the stock loot pipeline (open loot 8, move
    // to loot 7, ...) this same tick so whatever survived the filter is still
    // collected. This action sits just above that pipeline (relevance 9) so the
    // prune always runs first.
    return false;
}

// --- Advanced pulls -------------------------------------------------------
//
// The maneuver, top to bottom (leader-owned phase, read cross-bot by followers):
//
//   Idle      The tank glides the route normally. When a pullable pack comes
//             within DC_PULL_START_RANGE, pick a SAFE camp back along the already
//             -cleared route (ComputeSafeCamp keeps it clear of other packs and
//             bounds the drag), stamp it, halt the escort glide, -> Forming.
//   Forming   The tank holds where it committed (just outside aggro); the party
//             walks back to the camp and goes passive (hold-at-camp). The tank
//             waits here until the party is actually set at camp (or a timeout),
//             then -> Advancing. The party — not the tank — does the repositioning.
//   Advancing The tank tags the pack: a ranged class pull if it has one and is in
//             range+LOS, else it closes to body-tag. The moment combat starts,
//             control passes to DungeonClearPullManeuverAction on the combat engine.
//   Returning (combat engine) Drag the pack back to the waiting party at camp.
//   Engage    At camp: hand the fight to stock combat; ReapStrandedPassives sees
//             the non-holding phase and releases the party. Out of combat -> Idle.
//
// Camp is anchored in cleared ground BEHIND the tank (where the party already
// trails) rather than at the tank's forward commit spot, and is chosen so the
// fight can't aggro a neighbouring pack — so the tank pulls TOWARD the party
// instead of the party running out to the tank.
namespace
{
    // STATIC FALLBACK commit range (DC_PULL_START_RANGE) — the distance at which
    // the tank stops gliding, holds, and waits in Forming for the party to set at
    // the camp BEFORE it steps in to tag. The live value is normally
    // DcEngageGeometry::PullCommitRange, sized to the pack's REAL aggro radius so
    // the tank Forms just outside where it would face-pull; this constant only
    // applies when DynamicAggroRange is off or the target isn't a creature. It
    // still sits outside a same-level mob's ~20yd aggro so even the fallback
    // doesn't face-pull. Until the pack is this close Advance glides the tank in
    // (blocking-trash stands down in pull mode so it can't engage first). Defined
    // once in DungeonClearTuning.h (shared with the trigger).
    // Per-leg watchdog (tag / return) — abort a leg that makes no progress so a
    // navmesh wedge can never freeze the pull.
    constexpr uint32 DC_PULL_LEG_TIMEOUT_MS = 10000;
    // Consecutive fizzled pulls of the SAME pack (Engage cleanup found the pull
    // target alive and idle — the drag never delivered it) before the pack is
    // handed to the normal walk-in engage via abortTarget. Casters and planted
    // stragglers ignore the drag-back, evade home once the tank breaks LOS at
    // camp, and a re-pull just repeats the fizzle — the tank bouncing forward
    // and back while the party stands at camp never entering combat.
    constexpr uint32 DC_PULL_FIZZLE_MAX = 2;
    // Longest the tank waits in Forming for the party to park+go passive at camp
    // before tagging anyway. Sized to cover the party walking back to a far camp
    // (PullSetback can be tens of yards); past it the tank tags regardless and the
    // party finishes converging on camp during the drag-back. The between-pulls
    // gate already ensured the party was close and rested before the pull began.
    constexpr uint32 DC_PULL_PARTY_SET_TIMEOUT_MS = 8000;
    // How close to camp the tank must get on the return before releasing.
    constexpr float DC_PULL_CAMP_ARRIVE = 5.0f;
    // Follower camp tolerance — within this, hold; otherwise walk to camp.
    constexpr float DC_PULL_HOLD_RADIUS = 4.0f;
    // Tight settle tolerance to a follower's individual (fuzzed) camp slot. Much
    // smaller than the shared hold radius so the bot actually travels the last
    // 1-2yd onto its distinct slot — otherwise it would park the instant it
    // crossed the 4yd shared radius and the per-bot variance would never show.
    // The slot is navmesh-validated and players don't physically block one
    // another, so settling this close is always reachable.
    constexpr float DC_PULL_SLOT_RADIUS = 1.0f;
    // "Party is set" tolerance for the Forming gate. A touch wider than the hold
    // radius so a follower parked at the boundary reliably counts as set instead
    // of flickering in/out and never letting the tank tag.
    constexpr float DC_PULL_SET_RADIUS = 8.0f;

    void DcSetPullPhase(AiObjectContext* context, DcPullPhase p)
    {
        DcPullContext& pull = context->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
        if (p == DcPullPhase::Engage)
        {
            // Engage entry also clears the per-pull tag latch; the invariant
            // lives in DcPullContext::EnterEngage (shared with AbortLeaderPull).
            pull.EnterEngage(getMSTime());
            return;
        }
        pull.phase = p;
        pull.phaseSince = getMSTime();
    }

    // Why the leader tank can't carry out the pull drag-back right now, or nullptr
    // if it is fine. Hard loss-of-control (stun / fear / confuse) and root stop the
    // retreat outright; a heavy movement slow (run speed at/below `slowFloor` of
    // base) drags so slowly the pack wins the race home. Daze is already immunized
    // for the pull (DcLeaderSignal::SetLeaderDazeImmunity), so a slow seen here is a
    // genuine debuff — Hamstring, web, a Frostbolt chill. Read purely off Unit state
    // so no aura-id table is needed; the timing/grace decision lives in the
    // unit-tested DungeonClearMath::ShouldAbortPullForCc. The returned string is a
    // stable literal safe to log.
    char const* DcDragImpairReason(Player* tank, float slowFloor)
    {
        if (!tank)
            return nullptr;
        if (tank->HasUnitState(UNIT_STATE_STUNNED))
            return "stunned";
        if (tank->HasUnitState(UNIT_STATE_FLEEING))
            return "feared";
        if (tank->HasUnitState(UNIT_STATE_CONFUSED))
            return "confused";
        if (tank->IsRooted() || tank->HasUnitState(UNIT_STATE_ROOT))
            return "rooted";
        if (tank->GetSpeedRate(MOVE_RUN) <= slowFloor)
            return "slowed";
        return nullptr;
    }
}

bool DungeonClearPullAction::Execute(Event /*event*/)
{
    DcPullContext& pull = context->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    Position& camp = pull.camp;
    uint32 const since = pull.phaseSince;
    uint32 const now = getMSTime();
    DcPullPhase const phase = pull.phase;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");

    switch (phase)
    {
        case DcPullPhase::Idle:
        {
            // Begin a pull: choose a SAFE camp back along the cleared route, hold
            // here, and let the party reposition to it (the tank doesn't retreat).
            Unit* trash = next.has_value()
                ? DcTargeting::GetPullTarget(botAI) : nullptr;
            if (!trash)
            {
                DC_PULL_TRACE("[DC:{}] pull idle: no pull target -> yield to advance",
                              bot->GetName());
                return false;
            }

            // Patrol-wait hold (decision == 3): the governor has the pull held at
            // commit range to let a patrol clear before committing. Halt and own the
            // tick — the blocking/room-trash engages already stood down for decision
            // 3, so this just keeps the tank planted (no tag, no camp handshake)
            // until the governor flips the verdict (patrol passed -> LEEROY/ADVANCED)
            // or the wait times out. Followers trail normally (pull mode is off).
            if (pull.decision == 3u)
            {
                DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                DC_PULL_TRACE("[DC:{}] pull idle: holding for patrol ({:.1f}yd to pack)",
                              bot->GetName(), bot->GetExactDist2d(trash));
                return true;
            }
            // Don't commit until the pack is within reach (the trigger gates on the
            // same range). While it's farther, yield to Advance to glide closer —
            // but PUBLISH a prospective camp NOW so the party walks up to it IN
            // PARALLEL with the tank's approach, instead of holding at the stale
            // camp until the tank arrives and only THEN trudging forward (the old
            // robotic "tank scouts, wait, party moves, wait" stall the player saw).
            //
            // ComputeSafeCamp returns a point `setback` behind the tank along the
            // breadcrumb trail; as the tank glides in it creeps forward, so the
            // party trails at a roughly fixed standoff. We only ADOPT a candidate
            // that sits closer to the pack than the current camp (with a little
            // hysteresis), so the party advances monotonically and is never dragged
            // backward early in the glide — when "setback behind the tank" can still
            // fall behind the previous/seed camp. We still yield (return false) so
            // Advance keeps gliding the tank; the camp update is a pure side effect.
            float const toTrash = bot->GetExactDist2d(trash);
            // Commit only once the pack is within its OWN aggro range (+ margin), so
            // the tank stops to Form just outside where it would otherwise face-pull.
            float const commitRange =
                DcEngageGeometry::PullCommitRange(bot, trash, DC_PULL_START_RANGE);
            if (toTrash > commitRange)
            {
                // Room-wide-aggro pre-clear (RoomAggroRegistry): the pull is aimed
                // at a ROOM mob near a flagged boss, not a corridor pack. Yielding
                // to Advance here would glide the tank toward the BOSS and wake it
                // — exactly what the pre-clear exists to prevent. Instead publish a
                // prospective camp (so the party trails up in parallel) and walk
                // straight to the room-trash unit until it is within commit range,
                // then fall through to the normal Forming handshake below.
                if (DcTargeting::IsRoomClearActive(bot, context))
                {
                    float const sb = DcSettings::GetFloat(bot, "PullSetback");
                    float const sr = DcSettings::GetFloat(bot, "PullCampSafeRadius");
                    float const md = DcSettings::GetFloat(bot, "PullMaxDrag");
                    float clr = 0.0f, drg = 0.0f;
                    if (std::optional<Position> ahead = DcPullPlanner::ComputeSafeCamp(
                            botAI, trash, sb, sr, md, clr, drg))
                    {
                        bool const unset = camp.GetPositionX() == 0.0f &&
                                           camp.GetPositionY() == 0.0f &&
                                           camp.GetPositionZ() == 0.0f;
                        pull.campPublishedMs = now;
                        if (unset || ahead->GetExactDist2d(trash) + 3.0f <
                                         camp.GetExactDist2d(trash))
                            camp = *ahead;
                    }
                    // Walk to the room trash, skirting the boss's aggro sphere if
                    // the direct line clips it (RoomAggroSkirtPoint) — the same
                    // detour EngageDirect applies, so the pull-idle approach can't
                    // wake the boss either.
                    MoveToSkirtingRoomAggro(trash, MovementPriority::MOVEMENT_NORMAL);
                    DC_PULL_TRACE("[DC:{}] pull idle (room-clear): closing on room "
                                  "trash {} at {:.1f}yd (commit {:.1f})",
                                  bot->GetName(), trash->GetGUID().ToString(),
                                  toTrash, commitRange);
                    return true;
                }
                float const setback = DcSettings::GetFloat(bot, "PullSetback");
                float const safeRadius = DcSettings::GetFloat(bot, "PullCampSafeRadius");
                float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
                float clrAhead = 0.0f;
                float dragAhead = 0.0f;
                if (std::optional<Position> ahead = DcPullPlanner::ComputeSafeCamp(
                        botAI, trash, setback, safeRadius, maxDrag, clrAhead, dragAhead))
                {
                    bool const campUnset =
                        camp.GetPositionX() == 0.0f && camp.GetPositionY() == 0.0f &&
                        camp.GetPositionZ() == 0.0f;
                    float const candToTrash = ahead->GetExactDist2d(trash);
                    // +3yd hysteresis: only rewrite when the candidate is meaningfully
                    // more forward, so the party isn't churned by tick-to-tick jitter.
                    // A successful camp computation claims ownership for this
                    // tick even when the hysteresis below keeps the old point —
                    // "no change" is still an ownership decision, and Advance's
                    // scout-trailing must not wrestle the camp meanwhile (see
                    // campPublishedMs / DC_CAMP_PUBLISH_FRESH_MS).
                    pull.campPublishedMs = now;
                    if (campUnset || candToTrash + 3.0f < camp.GetExactDist2d(trash))
                    {
                        camp = *ahead;
                        DC_PULL_DEBUG("[DC:{}] pull idle: target {} at {:.1f}yd > start "
                                      "range {:.1f} -> party advances to camp "
                                      "({:.1f},{:.1f},{:.1f}) {:.1f}yd from pack while "
                                      "tank closes", bot->GetName(),
                                      trash->GetGUID().ToString(), toTrash,
                                      commitRange, camp.GetPositionX(),
                                      camp.GetPositionY(), camp.GetPositionZ(),
                                      candToTrash);
                        return false;
                    }
                }
                DC_PULL_TRACE("[DC:{}] pull idle: target {} at {:.1f}yd > start range "
                              "{:.1f} -> glide closer before committing",
                              bot->GetName(), trash->GetGUID().ToString(), toTrash,
                              commitRange);
                return false;
            }
            // Pick the camp: a generous distance back along the cleared route
            // (PullSetback) so the party gets real room, extended further only if
            // another pack is still within PullCampSafeRadius (ComputeSafeCamp).
            // Dungeon mobs have no leash, so far-back is good; PullMaxDrag is just a
            // sanity cap.
            float const setback = DcSettings::GetFloat(bot, "PullSetback");
            float const safeRadius = DcSettings::GetFloat(bot, "PullCampSafeRadius");
            float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
            float clearance = 0.0f;
            float drag = 0.0f;
            std::optional<Position> camped = DcPullPlanner::ComputeSafeCamp(
                botAI, trash, setback, safeRadius, maxDrag, clearance, drag);
            if (camped.has_value())
                camp = *camped;
            else
                camp = Position(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            pull.campPublishedMs = now;

            // Record whether this is a line-of-sight pull (ranged pack, camp placed
            // to break LOS) so the addon status line can announce it. Mirrors the
            // gate inside ComputeSafeCamp.
            pull.losPull = DcSettings::GetBool(bot, "PullRangedLosBreak") &&
                           DcEngageGeometry::IsRangedAttacker(bot, trash);

            // The pack this pull is about. Survives EnterEngage so the Engage
            // cleanup can detect a fizzled drag (target alive + idle after the
            // camp fight) and latch the pack over to the walk-in engage.
            pull.pullTarget = trash->GetGUID();

            // Halt the escort glide for real before committing. A plain StopMoving
            // does NOT cancel a launched escort spline (the door-blocked park is the
            // other witness), so without StopBot(Hold) the tank keeps gliding
            // forward past the commit spot and the camp is stamped behind a
            // position it's already leaving — the old overshoot / wrong-target pull.
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            // Square up on the pack for the dwell at the commit spot.
            DcFaceIfNeeded(bot, trash);
            DcSetPullPhase(context, DcPullPhase::Forming);
            // clearance == -1 means no other pack is anywhere near the camp.
            float const clrDisp =
                clearance >= std::numeric_limits<float>::max() ? -1.0f : clearance;
            size_t const trailLen = pull.breadcrumbs.size();
            DC_PULL_INFO("[DC:{}] advanced-pull plan: target {} at {:.1f}yd | camp "
                         "({:.1f},{:.1f},{:.1f}) drag {:.1f}yd | clearance {:.1f}yd "
                         "(safe {:.0f}, setback {:.0f}, trail {}) -> forming, "
                         "waiting for party",
                         bot->GetName(), trash->GetGUID().ToString(), toTrash,
                         camp.GetPositionX(), camp.GetPositionY(), camp.GetPositionZ(),
                         drag, clrDisp, safeRadius, setback, trailLen);
            return true;
        }

        case DcPullPhase::Forming:
        {
            // The tank HOLDS where it committed (just outside aggro). The party
            // walks back to the camp and goes passive (hold-at-camp). Tag only once
            // the party is actually set, so the pull never drags into open ground.
            DcMovement::StopBot(bot, DcMovement::Stop::Soft);
            // Keep facing the pack across the multi-second dwell (idempotent —
            // re-faces only if the pack repositioned us off-axis).
            DcFaceIfNeeded(bot, DcTargeting::GetPullTarget(botAI));

            bool const partySet =
                DcPullPlanner::IsPartySetAtCamp(bot, camp, DC_PULL_SET_RADIUS);
            bool const formingTimedOut = (now - since) > DC_PULL_PARTY_SET_TIMEOUT_MS;
            if (!partySet && !formingTimedOut)
            {
                DC_PULL_TRACE("[DC:{}] pull forming: waiting for party to set at camp "
                              "({}/{} ms)", bot->GetName(), now - since,
                              DC_PULL_PARTY_SET_TIMEOUT_MS);
                return true;
            }
            DcSetPullPhase(context, DcPullPhase::Advancing);
            DC_PULL_DEBUG("[DC:{}] pull forming complete ({}) -> advancing (tag)",
                          bot->GetName(),
                          partySet ? "party set" : "timed out waiting for party");
            return true;
        }

        case DcPullPhase::Advancing:
        {
            // Tag the pack. The moment combat starts the non-combat trigger stops
            // firing and DungeonClearPullManeuverAction drags it back to camp.
            Unit* trash = next.has_value() ? DcTargeting::GetPullTarget(botAI) : nullptr;
            if (!trash)
            {
                // Pack died / despawned before we tagged it — nothing to pull.
                DcSetPullPhase(context, DcPullPhase::Idle);
                DC_PULL_DEBUG("[DC:{}] pull advancing: target gone -> idle",
                              bot->GetName());
                return false;
            }

            if ((now - since) > DC_PULL_LEG_TIMEOUT_MS)
            {
                // Stalled without aggro (e.g. the tag resisted and we never closed).
                // Hand this pack to the normal walk-in engage so the run never hangs.
                pull.abortTarget = trash->GetGUID();
                DcSetPullPhase(context, DcPullPhase::Idle);
                DC_PULL_INFO("[DC:{}] advanced-pull: tag timed out (target {} at "
                             "{:.1f}yd) -> normal engage", bot->GetName(),
                             trash->GetGUID().ToString(), bot->GetExactDist(trash));
                return false;
            }

            // Prefer a RANGED tag: pull from spell range so the tank tags and the
            // pack comes to it, instead of running into the middle of the pack.
            ObjectGuid const lastPull = pull.tagTarget;
            if (DC_TRY_PULL_SPELL)
            {
                if (auto pick = ResolvePullSpell(botAI, bot))
                {
                    if (lastPull == trash->GetGUID())
                    {
                        // Already tagged — hold and let aggro flip us to the
                        // combat-engine drag-back. The leg timeout above is the
                        // backstop if the tag somehow drew no aggro.
                        DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                        DcFaceIfNeeded(bot, trash);
                        DC_PULL_TRACE("[DC:{}] pull advancing: tagged, holding for aggro "
                                      "({:.1f}yd to target)", bot->GetName(),
                                      bot->GetExactDist(trash));
                        return true;
                    }
                    float const d = bot->GetExactDist(trash);
                    if (d >= pick->minRange && d <= pick->maxRange &&
                        bot->IsWithinLOSInMap(trash))
                    {
                        bot->SetSelection(trash->GetGUID());
                        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, trash))
                            ServerFacade::instance().SetFacingTo(bot, trash);
                        if (botAI->CastSpell(pick->spellId, trash))
                        {
                            pull.tagTarget = trash->GetGUID();
                            DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                            DC_PULL_INFO("[DC:{}] advanced-pull: ranged tag spell {} at "
                                         "{:.1f}yd", bot->GetName(), pick->spellId, d);
                            return true;
                        }
                        // Cast failed (cooldown/silence) — fall through to body-tag.
                    }
                    // Not in range/LOS yet — fall through to close the gap; we'll
                    // re-enter and cast once within range and line of sight.
                }
            }

            // No ranged option (or out of range/LOS / cast failed): body-tag by
            // proximity. CRUCIAL: walk only to the EDGE of the pack's aggro bubble
            // and HOLD — let the mob notice and close the last few yards itself —
            // rather than sprinting (COMBAT priority is uninterruptible by combat
            // reflexes) all the way to the pack's centre. The old "MoveTo the mob's
            // exact position" arrived at the spawn before combat even registered, so
            // the drag-back (combat-engine only) took over far too late and an
            // un-trained tank face-pulled the whole pack and ate its opener. This
            // mirrors the ranged "tagged, hold for aggro" path above: stop just
            // inside aggro, let the pack come, then the maneuver drags it to camp.
            Creature* const trashCreature = trash->ToCreature();
            float const toTag = bot->GetExactDist(trash);

            // Distance at which the tank must stop to reliably tag. The core only
            // re-checks a pack's aggro on MOVEMENT (Creature::MoveInLineOfSight,
            // driven by relocation notifiers), and its notice test is the plain
            // CENTER-TO-CENTER distance vs Creature::GetAggroRange (CanStartAttack ->
            // IsWithinDistInMap with BOTH bounding radii excluded). So the tank has
            // to GLIDE to a point strictly INSIDE GetAggroRange — the moving approach
            // is what crosses the threshold and trips the notice. Stop ~2yd inside.
            //
            // The OLD formula ADDED both combat reaches to GetAggroRange, parking the
            // tank ~2yd OUTSIDE the real aggro bubble. Stationary there, no relocation
            // re-fired MoveInLineOfSight, the pack never noticed it, and the leg
            // watchdog timed out — the reported "advanced pull tag timed out" hang.
            float meleeReach = 0.0f;
            float tagStop = 0.0f;
            bool forceTag = false;   // close to body contact and actively swing
            if (trashCreature)
            {
                meleeReach = bot->GetCombatReach() + trash->GetCombatReach() + 1.0f;
                tagStop = trashCreature->GetAggroRange(bot) - 2.0f;
                // Creep the stop point inward the longer we go without aggro. A tank
                // parked exactly at the edge is never re-evaluated (no relocation ->
                // no MoveInLineOfSight), so stepping a little closer each tick is what
                // ultimately trips the notice. Ramps to body contact within ~2s, well
                // inside the leg watchdog, so a borderline stop can't hang the pull.
                uint32 const advancingMs = now - since;
                if (advancingMs > 1500)
                    tagStop -= ((advancingMs - 1500) / 1000.0f) * 3.0f;
                // Floor at body contact. If the pack's aggro is at/below melee (a
                // much-higher-level tank vs the core's 5yd minimum aggro), closing to
                // the edge can't cross it — go to contact and actively tag instead.
                if (tagStop <= meleeReach)
                {
                    tagStop = meleeReach;
                    forceTag = true;
                }
            }

            if (trashCreature && toTag <= tagStop)
            {
                if (forceTag && !bot->IsInCombat())
                {
                    // Pack too high-level to ever notice us on its own: force the tag
                    // with a melee swing so combat starts and the maneuver drag-back
                    // (combat engine) takes over.
                    bot->SetSelection(trash->GetGUID());
                    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, trash))
                        ServerFacade::instance().SetFacingTo(bot, trash);
                    bot->Attack(trash, true);
                    DC_PULL_TRACE("[DC:{}] pull advancing: force body-tag ({:.1f}yd)",
                                  bot->GetName(), toTag);
                    return true;
                }
                // Inside the aggro bubble — hold and let the pack close / flip the
                // engine to the maneuver drag-back. The leg timeout above is the
                // backstop if nothing aggros (resisted / non-hostile).
                DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                DcFaceIfNeeded(bot, trash);
                DC_PULL_TRACE("[DC:{}] pull advancing: at aggro edge ({:.1f}yd, "
                              "hold for aggro)", bot->GetName(), toTag);
                return true;
            }

            // Aim for a point tagStop yards out from the pack on the tank's side,
            // not the pack's centre, so the run stops at the aggro edge.
            float tagX = trash->GetPositionX();
            float tagY = trash->GetPositionY();
            float const tagZ = trash->GetPositionZ();
            if (trashCreature && toTag > 0.1f)
            {
                float const f = tagStop / toTag;
                tagX = trash->GetPositionX() + (bot->GetPositionX() - trash->GetPositionX()) * f;
                tagY = trash->GetPositionY() + (bot->GetPositionY() - trash->GetPositionY()) * f;
            }
            DC_PULL_TRACE("[DC:{}] pull advancing: closing to aggro edge ({:.1f}yd, "
                          "stop {:.1f})", bot->GetName(), toTag, tagStop);
            bool const moved = DcMoveTo(trash->GetMapId(), tagX, tagY, tagZ,
                                      /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                      /*exact_waypoint*/ false, MovementPriority::MOVEMENT_COMBAT);
            if (moved || bot->isMoving() || IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
                return true;

            // Couldn't move and not moving: navmesh wedge. Abort to normal engage.
            pull.abortTarget = trash->GetGUID();
            DcSetPullPhase(context, DcPullPhase::Idle);
            DC_PULL_INFO("[DC:{}] advanced-pull: tag navmesh-wedged ({:.1f}yd to "
                         "target) -> normal engage", bot->GetName(),
                         bot->GetExactDist(trash));
            return false;
        }

        case DcPullPhase::Returning:
            // Reached here only out of combat (the maneuver runs in combat): the
            // pack leashed / evaded mid-return. Release the party and reset.
            DcSetPullPhase(context, DcPullPhase::Engage);
            DC_PULL_INFO("[DC:{}] advanced-pull: out of combat mid-return (pack "
                         "leashed/evaded) -> release party", bot->GetName());
            return false;

        case DcPullPhase::Engage:
        default:
        {
            // Out-of-combat cleanup: the camp fight is over, ready the next pull.
            pull.abortTarget = ObjectGuid::Empty;

            // Engage-fizzle latch. The fight "ended" (tank out of combat) with
            // the pulled pack still alive and idle: the drag never delivered it
            // — a caster/planted straggler held its ground and evaded home the
            // moment the tank broke LOS at camp. Re-pulling repeats the exact
            // same fizzle (the tank bounces forward and back while the party
            // stands passive at camp, never entering combat), so after
            // DC_PULL_FIZZLE_MAX consecutive fizzles hand the pack to the
            // normal walk-in engage: blocking-trash exempts the abortTarget
            // from pull-mode standdown, the tank fights it where it stands, and
            // the leader-fight assist drives the party in. A pack that died or
            // is still being fought by the party resets the latch.
            Unit* pulled = pull.pullTarget.IsEmpty()
                ? nullptr : ObjectAccessor::GetUnit(*bot, pull.pullTarget);
            if (pulled && pulled->IsAlive() && !pulled->IsInCombat())
            {
                if (pull.fizzleTarget == pull.pullTarget)
                    ++pull.fizzleCount;
                else
                {
                    pull.fizzleTarget = pull.pullTarget;
                    pull.fizzleCount = 1;
                }
                // Health and distance discriminate WHY it fizzled: 100% health
                // means the pack never engaged (or fully reset behind the LOS
                // corner); reduced health means it fought and silently dropped
                // combat when its target became unreachable.
                if (pull.fizzleCount >= DC_PULL_FIZZLE_MAX)
                {
                    pull.abortTarget = pull.fizzleTarget;
                    DC_PULL_INFO("[DC:{}] advanced-pull: pull of {} fizzled {}x "
                                 "(alive and idle after the camp fight, {:.0f}% hp, "
                                 "{:.1f}yd) -> handing to normal engage",
                                 bot->GetName(), pull.fizzleTarget.ToString(),
                                 pull.fizzleCount, pulled->GetHealthPct(),
                                 bot->GetExactDist(pulled));
                }
                else
                    DC_PULL_DEBUG("[DC:{}] advanced-pull: pull of {} fizzled "
                                  "(alive and idle after the camp fight, {:.0f}% hp, "
                                  "{:.1f}yd, {}/{})", bot->GetName(),
                                  pull.pullTarget.ToString(), pulled->GetHealthPct(),
                                  bot->GetExactDist(pulled), pull.fizzleCount,
                                  DC_PULL_FIZZLE_MAX);
            }
            else
            {
                pull.fizzleTarget = ObjectGuid::Empty;
                pull.fizzleCount = 0;
            }
            pull.pullTarget = ObjectGuid::Empty;

            DcSetPullPhase(context, DcPullPhase::Idle);
            DC_PULL_DEBUG("[DC:{}] advanced-pull: camp fight done -> idle, ready for "
                          "next pull", bot->GetName());
            return false;
        }
    }
}

bool DungeonClearPullManeuverAction::Execute(Event /*event*/)
{
    DcPullContext& pull = context->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    Position& camp = pull.camp;
    uint32 const now = getMSTime();
    DcPullPhase const phase = pull.phase;

    // Keep the tank daze-proof for the whole drag. Immunity (set with pull mode)
    // should stop Daze landing at all; strip it here too as a backstop so a
    // retreat is never slowed to a crawl by a hit from behind.
    bot->RemoveAurasDueToSpell(1604);

    // A druid tank must take the run-home hits in bear form, not caster form.
    // Shapeshift is instant and not interrupted by the drag-back run, so refresh
    // it every tick of the maneuver (no-op once shifted / for non-druids).
    DcFollowerLifecycle::EnsureTankBearForm(bot);

    // First combat tick of the pull: aggro confirmed, turn around for camp.
    // Forming counts too — combat can be taken while the tank holds at the commit
    // spot waiting for the party to set (the pack wandered into it / a patrol).
    // Idle counts as well: an unplanned aggro while merely scouting toward the
    // next pack must ALSO retreat to the held party rather than fight in place.
    // Either way we drag back to the camp and release the party there rather than
    // letting the tank solo in place.
    if (phase == DcPullPhase::Advancing || phase == DcPullPhase::Forming ||
        phase == DcPullPhase::Idle)
    {
        // Unplanned aggro while scouting (Idle): stamp a FRESH safe camp just
        // behind the tank (away from the aggressor) and drag the pack THERE. The
        // party then converges on the NEW camp via hold-at-camp — we deliberately
        // do NOT haul the pack all the way back to the stale camp the party still
        // sits at. The fight happens on safe ground near where the aggro started
        // and the party comes up to it, even though it isn't there yet.
        // Forming/Advancing already carry the freshly-committed pull camp.
        if (phase == DcPullPhase::Idle)
        {
            Unit* aggressor = bot->GetVictim();
            for (Unit* a : bot->getAttackers())
            {
                if (!a || !a->IsAlive())
                    continue;
                if (!aggressor ||
                    bot->GetExactDist2d(a) < bot->GetExactDist2d(aggressor))
                    aggressor = a;
            }
            if (aggressor)
            {
                // Same fizzle bookkeeping as a planned pull: if this aggressor
                // evades home mid-drag over and over, the Engage cleanup must
                // see it and eventually hand it to the walk-in engage.
                pull.pullTarget = aggressor->GetGUID();

                float const setback = DcSettings::GetFloat(bot, "PullSetback");
                float const safeRadius = DcSettings::GetFloat(bot, "PullCampSafeRadius");
                float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
                float clr = 0.0f;
                float drag = 0.0f;
                if (std::optional<Position> fresh = DcPullPlanner::ComputeSafeCamp(
                        botAI, aggressor, setback, safeRadius, maxDrag, clr, drag))
                {
                    camp = *fresh;
                    pull.campPublishedMs = now;
                    DC_PULL_INFO("[DC:{}] advanced-pull: unplanned aggro while scouting "
                                 "-> fresh camp ({:.1f},{:.1f},{:.1f}) drag {:.1f}yd, "
                                 "party converges", bot->GetName(),
                                 camp.GetPositionX(), camp.GetPositionY(),
                                 camp.GetPositionZ(), drag);
                }
            }
        }

        // If the camp was never stamped and none could be computed, fall back to
        // fighting in place rather than dragging the pack to the map origin.
        if (camp.GetPositionX() == 0.0f && camp.GetPositionY() == 0.0f &&
            camp.GetPositionZ() == 0.0f)
        {
            camp = Position(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            pull.campPublishedMs = now;
        }

        // Stamp the return-leg length and clear the plant latch: the turn-and-plant
        // gate (below) requires at least half of THIS leg covered, and the debounce
        // must start fresh for the new drag.
        pull.returnLegStartDist = bot->GetExactDist(&camp);
        pull.plantTicks = 0;

        DcSetPullPhase(context, DcPullPhase::Returning);
        DC_PULL_INFO("[DC:{}] advanced-pull: aggro confirmed at {:.1f}yd from camp "
                     "(from {}) -> dragging to camp", bot->GetName(),
                     bot->GetExactDist(&camp),
                     phase == DcPullPhase::Forming ? "forming"
                         : phase == DcPullPhase::Idle ? "scouting" : "tag");
    }

    // CC-assist. If the tank is crowd-controlled mid-drag (stunned / feared /
    // confused / rooted, or slowed below PullCcSlowFloor) the retreat is failing —
    // it can't reach camp and just eats the pack while the party sits passive. Once
    // the CC has persisted past the grace, ABORT the pull: the tank STOPS the
    // run-home (cancelling the in-flight glide so it doesn't resume to camp when
    // the CC clears) and engages the pack where it stands, and the phase flips to
    // Engage — which both drops the party's passive camp-hold and trips
    // IsLeaderCampFightActive so the followers pile onto the pack and help. The
    // grace ignores a brief micro-CC so a single stutter-stun doesn't throw an
    // otherwise-fine pull away.
    if (DcSettings::GetBool(bot, "PullCcAssist"))
    {
        float const slowFloor = DcSettings::GetFloat(bot, "PullCcSlowFloor");
        char const* const ccReason = DcDragImpairReason(bot, slowFloor);
        uint32 const graceMs =
            uint32(DcSettings::GetFloat(bot, "PullCcAssistGrace") * 1000.0f);
        uint32 ccSinceOut = pull.ccSince;
        bool const ccAbort = DungeonClearMath::ShouldAbortPullForCc(
            ccReason != nullptr, pull.ccSince, now, graceMs, ccSinceOut);
        pull.ccSince = ccSinceOut;
        if (ccAbort)
        {
            pull.ccSince = 0;

            // Hard-cancel the run-home. The drag-back is a launched MoveTo
            // (MOVEMENT_COMBAT) glide; a plain StopMoving can leave it queued
            // UNDER the active CC generator, so the instant the impairment
            // wears off the tank resumes sprinting to the (now abandoned) camp
            // instead of fighting. StopBot(HardPin) drops any escort spline +
            // clears the LastMovement wait and pins the point-move on the spot
            // (unconditionally, since the bot may not be "moving" under CC).
            DcMovement::StopBot(bot, DcMovement::Stop::HardPin);

            DcSetPullPhase(context, DcPullPhase::Engage);

            // Start combat right here. Flipping to Engage already releases the
            // party (ReapStrandedPassives + IsLeaderCampFightActive), but the
            // tank itself must commit to the pack too: face and attack the
            // nearest attacker so it turns on the mob the moment the CC clears,
            // rather than drifting back toward camp before stock combat
            // re-acquires. Mirrors EngageDirect's in-range branch.
            Unit* aggressor = bot->GetVictim();
            for (Unit* a : bot->getAttackers())
            {
                if (!a || !a->IsAlive())
                    continue;
                if (!aggressor ||
                    bot->GetExactDist2d(a) < bot->GetExactDist2d(aggressor))
                    aggressor = a;
            }
            if (aggressor)
            {
                bot->SetSelection(aggressor->GetGUID());
                if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, aggressor))
                    ServerFacade::instance().SetFacingTo(bot, aggressor);
                context->GetValue<Unit*>("current target")->Set(aggressor);
                bot->Attack(aggressor, botAI->IsMelee(bot));
            }
            botAI->ChangeEngine(BOT_STATE_COMBAT);

            DC_PULL_INFO("[DC:{}] advanced-pull: tank {} mid-drag (held >{} ms) -> "
                         "abort pull, stop run-home + engage {}, releasing party "
                         "to assist", bot->GetName(), ccReason, graceMs,
                         aggressor ? aggressor->GetGUID().ToString() : "pack");
            return false;
        }
    }

    uint32 const since = pull.phaseSince;
    // `since` is stamped by DcSetPullPhase via its OWN getMSTime() call, which can
    // read a millisecond LATER than the `now` captured at the top of Execute. So on
    // the very tick we transition into Returning (above), now < since and a raw
    // `now - since` underflows to ~4.29e9 — instantly tripping the leg-timeout and
    // dumping the tank into "fight in place", which is why the pull-back to camp
    // worked or failed at random (a millisecond-boundary race). Clamp the elapsed.
    uint32 const legElapsed = now > since ? now - since : 0u;
    float const dist = bot->GetExactDist(&camp);

    if (dist <= DC_PULL_CAMP_ARRIVE)
    {
        // Back at camp: stop and hand the fight to stock combat. Flipping to
        // Engage is also what makes ReapStrandedPassives release the party.
        DcMovement::StopBot(bot, DcMovement::Stop::Soft);
        DcSetPullPhase(context, DcPullPhase::Engage);
        DC_PULL_INFO("[DC:{}] advanced-pull: at camp ({:.1f}yd) -> engaging, party "
                     "released", bot->GetName(), dist);
        return false;
    }

    if (legElapsed > DC_PULL_LEG_TIMEOUT_MS)
    {
        // Return leg wedged — fight where we are rather than freeze.
        DcSetPullPhase(context, DcPullPhase::Engage);
        DC_PULL_INFO("[DC:{}] advanced-pull: return leg wedged at {:.1f}yd from camp "
                     "after {} ms -> fighting in place",
                     bot->GetName(), dist, legElapsed);
        return false;
    }

    // Turn-and-plant. A human tank doesn't sprint the WHOLE leg back-turned; once
    // the pack is glued to it and chasing, it stops a few steps in, turns, and
    // fights — the fight happens wherever it actually plants. Stopping early is the
    // single biggest cut to the back-exposure window the daze-immunity cheat exists
    // to paper over. Suppressed for LOS-break pulls (the whole point is reaching
    // the corner) and gated on half the leg covered + a 2-tick debounce so a single
    // noisy distance read can't trip it. The plant point IS the new camp — re-stamp
    // it so the spread gate, status panel, and follower hold all follow the fight.
    if (DcSettings::GetBool(bot, "PullPlantEnable"))
    {
        float const glueRadius = DcSettings::GetFloat(bot, "PullPlantGlueRadius");
        std::vector<float> attackerDists;
        for (Unit* a : bot->getAttackers())
            if (a && a->IsAlive())
                attackerDists.push_back(bot->GetExactDist(a));

        if (DungeonClearMath::ShouldPlantEarly(attackerDists, glueRadius,
                /*glueTicksNeeded*/ 2u, pull.losPull, dist,
                pull.returnLegStartDist, pull.plantTicks))
        {
            // Hard-cancel the run-home exactly as the CC-abort branch does: a plain
            // StopMoving can leave the launched MOVEMENT_COMBAT glide queued and the
            // tank resumes sprinting to the abandoned camp instead of fighting.
            DcMovement::StopBot(bot, DcMovement::Stop::HardPin);

            // The camp IS wherever the fight lands. Re-stamp it (fresh publish) so
            // the party converges on the plant point and the addon panel relocates.
            camp = Position(bot->GetPositionX(), bot->GetPositionY(),
                            bot->GetPositionZ());
            pull.campPublishedMs = now;

            // Turn on the nearest attacker so the tank commits the moment it stops,
            // rather than drifting before stock combat re-acquires (mirrors the
            // CC-abort engage block).
            Unit* nearest = bot->GetVictim();
            for (Unit* a : bot->getAttackers())
            {
                if (!a || !a->IsAlive())
                    continue;
                if (!nearest ||
                    bot->GetExactDist2d(a) < bot->GetExactDist2d(nearest))
                    nearest = a;
            }
            if (nearest)
            {
                bot->SetSelection(nearest->GetGUID());
                if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, nearest))
                    ServerFacade::instance().SetFacingTo(bot, nearest);
                context->GetValue<Unit*>("current target")->Set(nearest);
                bot->Attack(nearest, botAI->IsMelee(bot));
            }

            DcSetPullPhase(context, DcPullPhase::Engage);
            botAI->ChangeEngine(BOT_STATE_COMBAT);

            DC_PULL_INFO("[DC:{}] advanced-pull: pack gathered at {:.1f}yd from camp "
                         "-> plant + engage", bot->GetName(), dist);
            return false;
        }
    }

    // Run to camp. Own the tick (return true even on a duplicate move) so stock
    // combat chase/attack can't grab the tank and fight at the pack instead.
    DC_PULL_TRACE("[DC:{}] pull returning: {:.1f}yd to camp ({} ms into leg)",
                  bot->GetName(), dist, legElapsed);
    DcMoveTo(bot->GetMapId(), camp.GetPositionX(), camp.GetPositionY(), camp.GetPositionZ(),
           /*idle*/ false, /*react*/ false, /*normal_only*/ false,
           /*exact_waypoint*/ false, MovementPriority::MOVEMENT_COMBAT);
    return true;
}

bool DungeonClearCampHoldActionBase::Execute(Event /*event*/)
{
    Position camp;
    bool passive = false;
    if (!DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
        return false;

    // Healers hold at camp like everyone else but are pinned with the "stay"
    // strategy instead of "+passive" (ApplyFollowerPassive) so they can heal the
    // tank through the drag-back without RUNNING FORWARD to close heal range —
    // "stay" suppresses playerbots' reach-to-heal movement while leaving the
    // cast-heal action free. The position pin below still applies — we only stop
    // OWNING the tick once they're parked AND someone needs a heal, so the combat
    // engine can run the in-place heal cast.
    bool const isHealer = PlayerbotAI::IsHeal(bot);

    // Go passive (attack nothing) ONLY while the tank is actually tagging (a
    // holding phase). While merely holding at camp between pulls the party stays
    // ready to defend; the reaper strips any DC passive once we leave a holding
    // phase. Idempotent; the matching release is centralized in
    // ReapStrandedPassives so it fires on every exit.
    if (passive)
        DcFollowerLifecycle::ApplyFollowerPassive(bot);

    // Loot yield (scout phase only). The pack dies AT camp, so its corpses sit
    // right where the party holds. Without this the camp hold and the stock loot
    // pipeline fight each other: move-to-loot walks a follower a few yards to a
    // corpse, hold-at-camp yanks it back (corpse just outside the hold radius),
    // and un-finishable / below-floor corpses keep the loot flags armed forever —
    // the ping-pong the player saw. Mirror the follow-tank loot yield exactly:
    // prune corpses we must not take (skipped / below DungeonClear.LootMinQuality /
    // camped too long), then step ASIDE (return false, don't pull back to camp)
    // while a takeable corpse is in progress, bounded by a commit timeout. Skip
    // entirely while the tank is tagging (passive) — the party must stay pinned.
    if (!passive)
    {
        DcLootPolicy::StripSkippedLoot(botAI);
        DcLootPolicy::MaybeSkipUnworthyLoot(botAI);
        DcLootPolicy::MaybeGiveUpCampedLoot(botAI, DC_LOOT_CAMP_TIMEOUT_MS,
                                                DC_LOOT_GIVEUP_TTL_MS);
        uint32& lootYieldStart =
            context->GetValue<DcApproachState&>("dungeon clear approach state")->Get().lootYieldStartMs;
        bool const lootYield =
            AI_VALUE(bool, "has available loot") || AI_VALUE(bool, "can loot");
        if (lootYield)
        {
            uint32 const nowMs = getMSTime();
            if (lootYieldStart == 0)
                lootYieldStart = nowMs;
            if (nowMs - lootYieldStart < DC_LOOT_YIELD_TIMEOUT_MS)
            {
                // Hand the tick to move-to-loot / open-loot; do NOT yank back to
                // camp (that is the ping-pong).
                DC_PULL_TRACE("[DC:{}] hold-at-camp yielding: loot in progress ({}ms)",
                              bot->GetName(), nowMs - lootYieldStart);
                return false;
            }
            // Waited long enough — blacklist THIS corpse so the flags drop and we
            // stop being drawn back to it, then resume holding at camp.
            DcLootPolicy::GiveUpCurrentLoot(botAI, DC_LOOT_GIVEUP_TTL_MS);
            DC_PULL_TRACE("[DC:{}] hold-at-camp loot-yield timed out -> giving up corpse",
                          bot->GetName());
        }
        else
        {
            lootYieldStart = 0;  // not looting -> reset the commit timer
        }
    }

    // Cancel any persistent MoveFollow that follow-tank installed before the
    // pull. While holding we own this bot's movement, but MoveFollow lives in
    // the same ACTIVE MotionMaster slot and StopMoving does NOT remove the
    // generator — it re-asserts on the next motion update and walks the
    // follower right back out after the advancing tank (this is exactly how the
    // party ended up trailing the tank to the mob). Clear it once; follow-tank
    // re-installs it when the pull releases. Same persistent-generator gotcha as
    // the DC-tank-gone teardown / [[selfbot-stale-movefollow]].
    if (bot->GetMotionMaster() &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        DcMovement::StopBot(bot, DcMovement::Stop::Hold);
        context->GetValue<ObjectGuid>("dungeon clear followed tank")->Set(ObjectGuid::Empty);
        DcFollowerLifecycle::UnmarkFollowing(bot->GetGUID());
    }

    // Held ranged healer: clamp its movement so it can APPROACH the tank to reach
    // heal range but NEVER cross to the threat side of it. The healer carries the
    // "stay" pin (ApplyFollowerPassive) which disables every stock mover — so on
    // the yield below the cast-heal fires but nothing can walk the bot, killing
    // the overshoot where a pure ranged healer ran past the tank to a heal/follow
    // point that sits behind the tank (and behind = toward the pack when the tank
    // faces camp on the drag-back). DC supplies the approach itself, clamped to
    // the camp side of the heal target, so "approach yes, never past the tank".
    if (passive && isHealer)
    {
        Unit* const healTarget = AI_VALUE(Unit*, "party member to heal");
        uint8 const lowestPct = AI_VALUE2(uint8, "health", "party member to heal");
        if (healTarget && lowestPct < sPlayerbotAIConfig.almostFullHealth)
        {
            float const healRange = botAI->GetRange("heal");
            bool const canCast = bot->GetExactDist(healTarget) <= healRange &&
                                 bot->IsWithinLOSInMap(healTarget);
            if (canCast)
            {
                // In range + LOS: hold here and YIELD so the (movement-suppressed)
                // cast-heal lands in place. "stay" guarantees no mover runs.
                DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                DC_PULL_TRACE("[DC:{}] healer: in range -> casting in place", bot->GetName());
                return false;
            }
            // Out of range/LOS: drive a clamped approach toward a point at ~85%
            // heal range on the CAMP side of the target. Own the tick (don't let
            // the camp slot yank it straight back) and use MOVEMENT_COMBAT so it
            // wins over any stale combat mover.
            Position const approach =
                DcPullPlanner::ComputeHealApproach(bot, healTarget, camp, healRange);
            if (bot->GetExactDist(&approach) > DC_PULL_SLOT_RADIUS)
            {
                DcMoveTo(bot->GetMapId(), approach.GetPositionX(), approach.GetPositionY(),
                       approach.GetPositionZ(), /*idle*/ false, /*react*/ false,
                       /*normal_only*/ false, /*exact_waypoint*/ false,
                       MovementPriority::MOVEMENT_COMBAT);
                DC_PULL_TRACE("[DC:{}] healer: clamped approach to heal target", bot->GetName());
                return true;
            }
            // At the clamped point but still no LOS/range (corner): hold, yield for
            // a possible cast — never push past the clamp toward the pack.
            DcMovement::StopBot(bot, DcMovement::Stop::Soft);
            return false;
        }
        // Nobody needs healing — fall through to the normal camp-slot pin so the
        // healer holds at / returns to camp like any other held follower.
    }

    // Park at the leader's camp. Each follower aims for its own fuzzed slot — a
    // deterministic 1-2yd offset off the shared anchor, snapped to the navmesh —
    // so the party fans out instead of stacking on one identical point. Settle on
    // the slot with a tight tolerance (the wide hold radius would let the bot stop
    // before the variance ever showed); fall back to the anchor when the slot
    // probe failed (slot == camp).
    Position const slot = DcPullPlanner::ComputeCampSlot(bot, camp);
    float const toCamp = bot->GetExactDist(&slot);
    if (toCamp <= DC_PULL_SLOT_RADIUS)
    {
        DcMovement::StopBot(bot, DcMovement::Stop::Soft);
        // A waiting party watches their tank work: face the LEADER (not the
        // pack) once parked — never while still walking to camp.
        DcFaceIfNeeded(bot, DcLeaderSignal::FindLeaderTank(bot));
        DC_PULL_TRACE("[DC:{}] hold-at-camp: parked ({:.1f}yd, passive={})",
                      bot->GetName(), toCamp, passive);
        // During a holding phase (the tank is tagging) OWN the tick so nothing can
        // break the hold while the pull is live. While merely camped between pulls
        // (scout phase, passive==false) YIELD so the party can rest / loot at camp
        // — the multiplier suppresses wander / follow / self-pull for a camp-held
        // follower, so yielding here can't let it drift off toward the tank. A
        // healer's in-place / clamped-approach healing is handled in the dedicated
        // block above and returns before reaching here; once nobody needs a heal it
        // falls through to this camp pin like any other held follower.
        return passive;
    }
    // Priority matters the moment the party is already IN COMBAT when a pull
    // commits — e.g. a dynamic LEEROY->ADVANCED upgrade that lands after the tank
    // (and the following party) already ran into the pack. During a holding phase
    // (passive) the party MUST retreat to the camp even mid-fight, so move at
    // MOVEMENT_COMBAT — the same priority the tank's drag-back uses — otherwise a
    // MOVEMENT_NORMAL move loses to the stock combat MoveChase generator already
    // installed on the engaged follower and it just DPSes the pack where it stands
    // (the "party didn't run back, chaos" case). While merely scouting between pulls
    // (passive == false, usually out of combat) NORMAL is right: it must NOT stomp
    // the loot/rest pipeline the party runs at camp.
    MovementPriority const prio = passive ? MovementPriority::MOVEMENT_COMBAT
                                          : MovementPriority::MOVEMENT_NORMAL;
    bool const moved =
        DcMoveTo(bot->GetMapId(), slot.GetPositionX(), slot.GetPositionY(), slot.GetPositionZ(),
               /*idle*/ false, /*react*/ false, /*normal_only*/ false,
               /*exact_waypoint*/ false, prio);
    DC_PULL_TRACE("[DC:{}] hold-at-camp: walking to camp ({:.1f}yd, passive={}, moved={})",
                  bot->GetName(), toCamp, passive, moved);
    return true;
}

bool DungeonClearAssistCampActionBase::Execute(Event /*event*/)
{
    Player* leader = DcLeaderSignal::FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;

    // Nearest live unit the leader is meleeing — the pulled pack. LOS-blind on
    // purpose: this action exists precisely for the case the drag-back parked the
    // pack out of the camp's line of sight, where the stock target picker never
    // acquires it. getAttackers() covers the melee pack the tank is holding; the
    // leader's victim is the fallback for the (rare) all-ranged grab.
    Unit* target = nullptr;
    float bestDist = 0.0f;
    for (Unit* a : leader->getAttackers())
    {
        if (!a || !a->IsAlive() || a->GetMapId() != bot->GetMapId())
            continue;
        if (!bot->IsValidAttackTarget(a))
            continue;
        float const d = bot->GetExactDist2d(a);
        if (!target || d < bestDist)
        {
            target = a;
            bestDist = d;
        }
    }
    if (!target)
        target = leader->GetVictim();

    // No concrete target resolvable (e.g. brief threat-table gap): close on the
    // leader instead, which puts us back in sight of whatever it is fighting so
    // stock combat can pick the target up the moment we round the corner.
    Unit* const moveTo = target ? target : static_cast<Unit*>(leader);

    if (target)
    {
        // Commit the follower to the pack even without LOS: select it, publish it
        // as the current target, and open a combat timer so the bot is in the
        // fight (and in the combat engine) the instant movement carries it back
        // into sight — instead of standing idle at camp.
        bot->SetSelection(target->GetGUID());
        context->GetValue<Unit*>("current target")->Set(target);
        if (!bot->IsInCombat())
            bot->SetInCombatWith(target);

        // Already in sight of the pack: don't lurch the body onto the mob. The
        // SetInCombatWith above flips the bot into the combat engine next tick,
        // where its own rotation/heal logic (un-suppressed there, unlike the stock
        // proactive pickers DC mutes for followers) handles positioning. This is
        // the case the non-combat assist now covers that it used to drop: an idle
        // follower WITH line of sight that DC's multiplier otherwise leaves stuck.
        if (bot->IsWithinLOSInMap(target))
        {
            DC_PULL_TRACE("[DC:{}] assist camp: in sight of pack ({:.1f}yd) -> "
                          "committed + forced combat, rotation takes over",
                          bot->GetName(), bot->GetExactDist(target));
            return true;
        }
    }

    // Band the approach priority exactly as EngageDirect does. COMBAT priority
    // can't be interrupted by the bot's combat reflexes, so on a LONG assist run
    // (the follower is far behind because the tank scouted/Leeroy-charged well
    // ahead, or it is out of LOS at the far end of a corridor) an unconditional
    // COMBAT move makes the follower plow straight through every pack it aggros
    // en route without stopping to fight — dragging a mob train to the far point
    // and wiping the group. Use COMBAT only for the final close approach; past
    // that, NORMAL, so the follower stops and fights what it pulls on the way in
    // (stock combat takes the intervening pack; the assist re-fires and resumes
    // once it dies — a controlled leapfrog instead of a runaway charge).
    float const distance = bot->GetExactDist(moveTo);
    float const attackRange = botAI->IsMelee(bot)
        ? (bot->GetCombatReach() + moveTo->GetCombatReach() + 1.0f)
        : (botAI->GetRange("spell") - CONTACT_DISTANCE);
    MovementPriority const prio =
        (distance <= attackRange + DC_COMBAT_APPROACH_RANGE)
            ? MovementPriority::MOVEMENT_COMBAT
            : MovementPriority::MOVEMENT_NORMAL;

    DC_PULL_TRACE("[DC:{}] assist camp: closing on out-of-LOS fight ({:.1f}yd, "
                  "target={}, prio={})", bot->GetName(), distance,
                  target ? target->GetGUID().ToString() : "leader",
                  prio == MovementPriority::MOVEMENT_COMBAT ? "combat" : "normal");

    DcMoveTo(moveTo->GetMapId(), moveTo->GetPositionX(), moveTo->GetPositionY(),
           moveTo->GetPositionZ(), /*idle*/ false, /*react*/ false,
           /*normal_only*/ false, /*exact_waypoint*/ false, prio);
    return true;
}

bool DungeonClearRegroupCombatAction::Execute(Event /*event*/)
{
    // The leader tank, non-null only on an active run (gated again by the trigger).
    Player* tank = AI_VALUE(Player*, "dungeon clear party tank");
    if (!tank || tank == bot)
        return false;

    // Close on the tank, but stop a few yards short rather than stacking on its
    // cell — enough to restore line of sight and heal/cast range without crowding
    // it (or piling a ranged class into melee/AoE). Pathing rounds corners, which
    // is exactly what regains a stranded healer's sight of its heal target. Once
    // the bot is back inside the leash (and, for a healer, back in LOS) the trigger
    // goes inert and stock combat rotation owns the tick again.
    constexpr float standoff = 5.0f;
    float const dist = bot->GetExactDist2d(tank);

    float x = tank->GetPositionX();
    float y = tank->GetPositionY();
    float const z = tank->GetPositionZ();
    if (dist > standoff)
    {
        float const frac = (dist - standoff) / dist;
        x = bot->GetPositionX() + (tank->GetPositionX() - bot->GetPositionX()) * frac;
        y = bot->GetPositionY() + (tank->GetPositionY() - bot->GetPositionY()) * frac;
    }

    // Band the priority like EngageDirect / the camp assist: COMBAT only for the
    // final close approach, NORMAL beyond. An unconditional COMBAT regroup runs a
    // stranded follower (a healer that lost LOS while the tank pushed far ahead)
    // straight through any packs between it and the tank without stopping to
    // fight — the same plow-through runaway. NORMAL on the long leg lets it break
    // off and clear what it aggros, then resume regrouping once that mob dies.
    float const toDest = bot->GetExactDist2d(x, y);
    MovementPriority const prio =
        (toDest <= DC_COMBAT_APPROACH_RANGE)
            ? MovementPriority::MOVEMENT_COMBAT
            : MovementPriority::MOVEMENT_NORMAL;

    DC_PULL_TRACE("[DC:{}] regroup: closing on tank {} ({:.1f}yd, los={}, prio={})",
                  bot->GetName(), tank->GetName(), dist,
                  bot->IsWithinLOSInMap(tank) ? 1 : 0,
                  prio == MovementPriority::MOVEMENT_COMBAT ? "combat" : "normal");

    DcMoveTo(tank->GetMapId(), x, y, z, /*idle*/ false, /*react*/ false,
           /*normal_only*/ false, /*exact_waypoint*/ false, prio);
    return true;
}
