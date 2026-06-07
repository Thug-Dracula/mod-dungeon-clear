/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARSTATEVALUES_H
#define _PLAYERBOT_DUNGEONCLEARSTATEVALUES_H

#include <map>
#include <string>
#include <unordered_set>

#include "ObjectGuid.h"
#include "Position.h"
#include "Value.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"

class PlayerbotAI;

class DungeonClearEnabledValue : public ManualSetValue<bool>
{
public:
    DungeonClearEnabledValue(PlayerbotAI* botAI) : ManualSetValue<bool>(botAI, false, "dungeon clear enabled") {}
};

// Pause flag layered on top of `dungeon clear enabled`. When true, every
// driving gate (the trigger ladder's IsEnabled, the multiplier, the
// follow-tank party-tank lookup, the blocking-door scan) goes inert so the
// tank behaves exactly as it does under `dc off` — it stops navigating,
// releases its followers, and lets stock wandering resume. Unlike `dc off`,
// `enabled` and all progress state (selected boss, skipped set, sticky boss)
// are preserved, so toggling pause back off resumes on the same boss. Always
// reset to false alongside `enabled` (dc on / dc off / death / cleared) so a
// fresh run can never start paused.
class DungeonClearPausedValue : public ManualSetValue<bool>
{
public:
    DungeonClearPausedValue(PlayerbotAI* botAI) : ManualSetValue<bool>(botAI, false, "dungeon clear paused") {}
};

// Short human phrase describing WHY the run is paused, so the status panel can
// tell the player whether they paused it themselves or whether the tank
// auto-paused on a door it can't open. Set at each of the two pause sites
// (DcPauseAction for a manual pause, DungeonClearDoorBlockedAction for the
// door auto-pause) the moment `dungeon clear paused` flips true; read by
// BuildStatusPayload only while paused. Cleared alongside the paused flag on
// resume / dc on / dc off / death / cleared so a stale reason can't leak into
// the next pause. Empty falls back to a generic "holding position".
class DungeonClearPauseReasonValue : public ManualSetValue<std::string&>
{
public:
    DungeonClearPauseReasonValue(PlayerbotAI* botAI)
        : ManualSetValue<std::string&>(botAI, data, "dungeon clear pause reason")
    {
    }

private:
    std::string data;
};

// GUID of the closed door the tank auto-paused in front of (see
// DungeonClearDoorBlockedAction's can't-open branch). Empty unless the run is
// paused specifically for an unopenable door. While it is set,
// DungeonClearDoorReopenedTrigger polls this one door every tick; the moment it
// reads OPEN — a human walked up and opened it — or it despawns/unresolves, the
// trigger auto-resumes the clear so the player doesn't ALSO have to hit Resume.
// Stamped ONLY by the door auto-pause site: a manual `dc pause` deliberately
// leaves it empty so opening some unrelated door can never auto-resume a
// hand-held pause. Cleared alongside the paused flag on resume / dc on / dc off
// / death / cleared so a stale door can't trigger a phantom resume next pause.
class DungeonClearPausedDoorValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearPausedDoorValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear paused door")
    {
    }
};

class DungeonClearSkippedValue : public ManualSetValue<std::unordered_set<uint32>&>
{
public:
    DungeonClearSkippedValue(PlayerbotAI* botAI)
        : ManualSetValue<std::unordered_set<uint32>&>(botAI, data, "dungeon clear skipped")
    {
    }

private:
    std::unordered_set<uint32> data;
};

// Boss entries that have been observed alive on the map at least once during
// the current run. This is what lets the status report tell a boss that "was
// alive and is now gone" (missing) apart from one we simply haven't reached
// yet — a boss in a far grid that has never loaded looks identical to a
// vanished one (neither is in the creature store), so without a memory of what
// we've actually seen alive, every unreached boss would falsely read as
// missing. Populated by DcBossesAction whenever a boss is found alive; cleared
// on `dc on` alongside the skipped set so a fresh run starts with a clean slate.
class DungeonClearSeenBossesValue : public ManualSetValue<std::unordered_set<uint32>&>
{
public:
    DungeonClearSeenBossesValue(PlayerbotAI* botAI)
        : ManualSetValue<std::unordered_set<uint32>&>(botAI, data, "dungeon clear seen bosses")
    {
    }

    void Reset() override { data.clear(); }

private:
    std::unordered_set<uint32> data;
};

class DungeonClearStuckCountValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStuckCountValue(PlayerbotAI* botAI) : ManualSetValue<uint32>(botAI, 0u, "dungeon clear stuck count") {}
};

class DungeonClearLastTargetEntryValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLastTargetEntryValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear last target entry")
    {
    }
};

class DungeonClearStallReasonValue : public ManualSetValue<std::string&>
{
public:
    DungeonClearStallReasonValue(PlayerbotAI* botAI)
        : ManualSetValue<std::string&>(botAI, data, "dungeon clear stall reason")
    {
    }

private:
    std::string data;
};

class DungeonClearLastSaidReasonValue : public ManualSetValue<std::string&>
{
public:
    DungeonClearLastSaidReasonValue(PlayerbotAI* botAI)
        : ManualSetValue<std::string&>(botAI, data, "dungeon clear last said reason")
    {
    }

private:
    std::string data;
};

// Short token describing what the advance action did on its most recent tick:
// "moving" (gliding the planned route), "pursuing" (closing directly on a live
// boss / final approach), or "recovering" (wedged off the route and repathing).
// Empty when the tank isn't actively navigating. DcStatusAction reads it to
// report a fine-grained activity to the companion addon — the navigation
// sub-states the coarse poll-time conditions (combat/loot/rest/stall) can't see
// on their own. Stamped every advance tick so the ~2s status poll always reads
// a fresh value; cleared on dc on / disable so a stale phase can't leak across
// runs. Distinct from the user-facing addon state: a "route not built yet"
// reading is derived at poll time, not stored here.
class DungeonClearPhaseValue : public ManualSetValue<std::string&>
{
public:
    DungeonClearPhaseValue(PlayerbotAI* botAI)
        : ManualSetValue<std::string&>(botAI, data, "dungeon clear phase")
    {
    }

private:
    std::string data;
};

class DungeonClearFallbackTargetValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearFallbackTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear fallback target")
    {
    }
};

// Sampled position from the previous DungeonClearAdvanceAction tick. Used
// alongside `dungeon clear stuck ticks` to detect geometric stuck-ness —
// the bot's MoveTo keeps returning true but its actual world position
// isn't changing. Default (0,0,0) is the "not yet sampled" sentinel since
// no real dungeon map has a (0,0,0) walkable point.
class DungeonClearLastPositionValue : public ManualSetValue<Position&>
{
public:
    DungeonClearLastPositionValue(PlayerbotAI* botAI)
        : ManualSetValue<Position&>(botAI, data, "dungeon clear last position")
    {
    }

private:
    Position data;
};

class DungeonClearStuckTicksValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStuckTicksValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear stuck ticks")
    {
    }
};

// Suppresses repeated pull-spell casts at the same engagement. Set when
// EngageDirect fires a class pull spell on a target; cleared when the
// target changes or the bot leaves combat.
class DungeonClearLastPullTargetValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearLastPullTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear last pull target")
    {
    }
};

// Sticky target for the blocking-trash engage action. Without this the
// per-tick re-scan in DungeonClearEngageTrashAction will flip between two
// roughly-equidistant corridor mobs as the bot moves, leaving it bouncing
// in place issuing MoveTo to each in turn and never reaching either. The
// stored GUID is kept across ticks as long as the unit is still alive,
// hostile, and on the bot's map; only invalidated when the unit dies,
// despawns, or the strategy itself is reset (dc on/off, party death, etc.).
class DungeonClearEngageTrashTargetValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearEngageTrashTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear engage trash target")
    {
    }
};

// GUID of the tank this (non-tank) bot is currently follow-following during
// dungeon clear. Set by DungeonClearFollowTankAction each time it issues the
// continuous MoveFollow; reset to empty once the follow has been torn down.
// Needed because MoveFollow is a persistent MotionMaster generator: when the
// DC tank goes away (dc off / death / all-cleared), the follow-tank trigger
// stops being selected and nothing would otherwise cancel the leftover
// "chase the tank" order. A self-bot can't self-heal that the way a normal
// bot does (its ordinary follow targets itself and no-ops without clearing),
// so it stays glued to the tank. This GUID lets the trigger fire one final
// teardown tick to Clear() the generator. Empty = not currently following a
// DC tank, nothing to tear down.
class DungeonClearFollowedTankValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearFollowedTankValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear followed tank")
    {
    }
};

// Boss entry NextDungeonBossValue committed to on its previous computation
// (0 = none). Read back as a commit-and-hold anchor: the value keeps returning
// the same boss until it leaves the candidate set (killed, skipped, or
// despawned), with no per-tick distance re-ranking or reachability re-probe.
// Without it the nearest-boss pick flip-flops between two roughly-equidistant
// bosses (branching routes, two wings) as the bot moves and the bounded
// reachability probe flickers, wedging the bot between competing long-path
// routes — the same jitter DungeonClearEngageTrashTargetValue solves for trash.
// Self-correcting — a stale entry from a prior run is simply ignored once it's
// no longer among the live/unspawned candidates.
class DungeonClearStickyBossValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStickyBossValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear sticky boss")
    {
    }
};

// Boss entry representing a manually selected boss override.
// 0 means no override active; normal automatic progression runs.
class DungeonClearSelectedBossValue : public ManualSetValue<uint32>
{
public:
    DungeonClearSelectedBossValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear selected boss")
    {
    }
};


// Boss entry that the cached `dungeon clear long path` was built for.
// 0 means "no cached path" — Advance/triggers rebuild on next tick.
// Boss-change, dc-skip, and dc-on all clear this to force a fresh build.
class DungeonClearLongPathTargetValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLongPathTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear long path target")
    {
    }
};

// World position the cached `dungeon clear long path` was actually built
// toward. For a pool-spawn / wandering boss (e.g. the Wailing Caverns
// Disciples) the live creature is rarely at its static DB spawn anchor, so
// Advance feeds EnsureLongPath the boss's LIVE position; this records where
// that path aims so a later tick can detect the boss has relocated (or that
// the live position has just streamed in, far from the static anchor the
// first build used) and force an early rebuild instead of walking a stale
// route the full TTL. Default (0,0,0) → "no path built yet".
class DungeonClearLongPathTargetPosValue : public ManualSetValue<Position&>
{
public:
    DungeonClearLongPathTargetPosValue(PlayerbotAI* botAI)
        : ManualSetValue<Position&>(botAI, data, "dungeon clear long path target pos")
    {
    }

private:
    Position data;
};

// Millisecond timestamp at which the cached LongPath expires. The cache
// stays bounded so a stale path doesn't survive past edge cases like map
// reloads; it is also rebuilt early when the live boss relocates far from
// the position the path was built toward (see long path target pos).
// Default 0 → "not cached yet".
class DungeonClearLongPathExpiresValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLongPathExpiresValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear long path expires")
    {
    }
};

// Async-pathfinding bookkeeping (DungeonClear.AsyncPathfinding). When the
// long-path build is offloaded to DcPathWorker, this holds the in-flight
// jobId (0 = none). EnsureLongPath polls DcPathWorker::TryTake(jobId) each
// tick and gates resubmission on this being non-zero, guaranteeing exactly
// one outstanding build per bot. The boss entry + map id the job was built
// for ride along in the worker's result (checked for staleness on take), so
// no separate per-bot copy of them is needed here. Cleared on install, on
// dc-off/skip/boss reset, and on a stale completion.
class DungeonClearPendingPathJobValue : public ManualSetValue<uint64>
{
public:
    DungeonClearPendingPathJobValue(PlayerbotAI* botAI)
        : ManualSetValue<uint64>(botAI, 0u, "dungeon clear pending path job")
    {
    }
};

// Millisecond timestamp of when the in-flight async job was submitted. Used as
// a watchdog: if no result arrives within DC_ASYNC_PATH_PENDING_TIMEOUT_MS the
// job is abandoned and rebuilt synchronously, so a lost result (swept after an
// afk dc-off) or a wedged worker can't leave the bot stuck plotting forever.
class DungeonClearPendingPathSinceValue : public ManualSetValue<uint32>
{
public:
    DungeonClearPendingPathSinceValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear pending path since")
    {
    }
};

// Index of the segment in the cached LongPath the bot is currently
// walking toward. Reset to 0 whenever the path target changes.
// Stored separately from the path so the segment list itself stays
// immutable while caching.
class DungeonClearCurrentHopValue : public ManualSetValue<uint32>
{
public:
    DungeonClearCurrentHopValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear current hop")
    {
    }
};

// Consecutive forward-recovery rebuild attempts. Incremented when Advance
// invalidates the long-path cache because the bot stopped making forward
// progress; reset to 0 the moment movement resumes. After several
// consecutive attempts without progress Advance escalates to a navmesh
// nudge — same FARFROMPOLY recovery the off-mesh case uses.
class DungeonClearStrideRebuildAttemptsValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStrideRebuildAttemptsValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear stride rebuild attempts")
    {
    }
};

// Millisecond timestamp at which Advance first began yielding for an
// in-progress loot pickup (0 = not currently loot-yielding). Drives the
// commit-timeout: Advance yields to let the loot system pick up a corpse,
// but only for DC_LOOT_YIELD_TIMEOUT_MS; past that it force-advances past
// a corpse it can't finish looting (group-loot rolls pending, bags full)
// so the tank never parks on it forever. Reset to 0 the moment the loot
// flags clear (corpse looted, or walked out of lootDistance).
class DungeonClearLootYieldStartValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLootYieldStartValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear loot yield start")
    {
    }
};

// Consecutive Advance ticks on which the long-path completed (cursor reached
// the polyline end) while the bot was still outside engage range of the boss.
// This is the "route dead-ends short of the boss" wedge: the navmesh can't get
// within DC_ENGAGE_RANGE (boss on a ledge / across a gap, or a wall-screened
// route that can't close the last yards), so NextHop reports done every tick
// and Advance rebuilds an identical 0-point path forever. The bot isn't moving
// in that loop, so the position-based stuck counter never catches it. Advance
// counts these ticks, makes a few straight-line final-approach attempts, then
// escalates to a stall. Reset on boss change and once back inside engage range.
class DungeonClearDoneNotEngagedTicksValue : public ManualSetValue<uint32>
{
public:
    DungeonClearDoneNotEngagedTicksValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear done-not-engaged ticks")
    {
    }
};

// Consecutive Advance ticks on which the LIVE-boss direct-pursuit branch issued
// a MoveTo that produced no movement at all (MoveTo returned false and the bot
// is not moving / not waiting on an in-flight move). This is the silent freeze
// just outside pull range: the boss is close and in line of sight, so direct
// pursuit is selected, but PathGenerator can't reach its live poly (Z resolves
// to INVALID_HEIGHT, or the route winds past PathGenerator's 74-hop cap). The
// bot never moves, so the position-based stuck counter can't see it and direct
// pursuit would retry forever. Advance counts these ticks and, past a short
// grace, abandons direct pursuit and falls through to the wall-screened
// long-path (LongRangePathfinder, no hop cap, with its own dead-end -> stall
// escalation). Reset on boss change and whenever the pursuit move makes
// progress / is in flight.
class DungeonClearPursuitFailTicksValue : public ManualSetValue<uint32>
{
public:
    DungeonClearPursuitFailTicksValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear pursuit fail ticks")
    {
    }
};

// Per-corpse loot give-up list: maps a loot GUID whose pickup the bot
// abandoned to the ms timestamp at which the skip expires — or to
// DungeonClearUtil::LOOT_SKIP_STICKY (0) for a permanent skip. Each DC tick the
// still-live entries are stripped from the stock "available loot" stack and
// cleared from "loot target" (see DungeonClearUtil::StripSkippedLoot), so
// neither the loot flags nor stock's nearest-target pick can re-commit to it.
// This is what breaks the corpse<->tank / chest<->path ping-pong that arose
// when un-finishable loot kept re-arming the loot yield every time the bot
// drifted back within LootDistance. The ttl depends on WHY the corpse was
// skipped. Permanent reasons (empty / below the quality floor / roll-locked or
// won-by-another — the winner's item auto-delivers, never re-looted — round-
// robin or allowed-looter excluding us, or bags full with no mid-dungeon
// vendor) are recorded sticky by the content-inspecting MaybeSkipUnworthyLoot
// and never re-arm on a backtrack. A real expiry is used only for the residual
// cases that inspection can't pre-classify — a camp / 15s-yield timeout on loot
// that looked takeable but didn't complete (a momentary second looter, own loot
// not yet reached) — so it is retried once rather than abandoned outright. All
// entries — sticky included — are cleared on dc on/off / party death via
// DisableDungeonClear.
class DungeonClearLootSkipValue : public ManualSetValue<std::map<ObjectGuid, uint32>&>
{
public:
    DungeonClearLootSkipValue(PlayerbotAI* botAI)
        : ManualSetValue<std::map<ObjectGuid, uint32>&>(botAI, data, "dungeon clear loot skip")
    {
    }

    void Reset() override { data.clear(); }

private:
    std::map<ObjectGuid, uint32> data;
};

// The loot GUID the bot is currently "camped" on — within interaction range
// (can-loot true) of one specific corpse — together with the ms timestamp at
// which it arrived. DungeonClearUtil::MaybeGiveUpCampedLoot uses the pair to
// time how long the bot has stood on the SAME corpse: a normal loot
// transaction clears in a tick or two, so camping a plain (non-gathering)
// corpse much longer means its loot is un-finishable (group-roll items pending
// a real player's roll, items reserved for others, bags full). Past the camp
// cutoff the corpse is blacklisted at once instead of burning the full
// loot-yield timeout on it — the "stuck on certain corpses" stall. Empty / 0
// whenever the bot isn't standing on a corpse (resets every tick can-loot is
// false), so a new corpse starts a fresh clock.
class DungeonClearLootCampGuidValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearLootCampGuidValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear loot camp guid")
    {
    }
};

class DungeonClearLootCampStartValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLootCampStartValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear loot camp start")
    {
    }
};

// Cursor into the cached long-path's flattened polyline plus the
// off-path tick counter. Reset whenever the path is rebuilt (boss
// change, TTL expiry, forced rebuild). The follower keeps this in sync
// each Advance tick; nothing else writes to it.
class DungeonClearFollowerStateValue : public ManualSetValue<DungeonFollowerState&>
{
public:
    DungeonClearFollowerStateValue(PlayerbotAI* botAI)
        : ManualSetValue<DungeonFollowerState&>(botAI, data, "dungeon clear follower state")
    {
    }

    void Reset() override { data = DungeonFollowerState{}; }

private:
    DungeonFollowerState data;
};

#endif
