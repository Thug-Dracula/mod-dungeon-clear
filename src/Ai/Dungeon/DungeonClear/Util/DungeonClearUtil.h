/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARUTIL_H
#define _PLAYERBOT_DUNGEONCLEARUTIL_H

#include <optional>
#include <string>
#include <vector>

#include "Log.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
// Umbrella: units split out of the former DungeonClearUtil god-class. Files that
// include DungeonClearUtil.h keep resolving DcEngageGeometry::/DcTargeting::/...
// without per-file include churn during the decomposition.
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyState.h"

// --- Advanced-pull log channel --------------------------------------------
// Pull mode (LOS pull-to-camp) gets its OWN log category, a child of the main
// DungeonClear channel (playerbots.dungeonclear.pull). It inherits the parent
// appender by default, but can be split to its own file and raised to Debug /
// Trace independently — without flooding the main channel — via
// Logger.playerbots.dungeonclear.pull in mod_dungeon_clear.conf. The feature is
// young and chatty to debug, so every pull state-machine transition, camp
// decision, leg watchdog, and passive-management step routes through these.
//
//   DC_PULL_INFO  — milestones a maintainer wants even at the default level
//                   (camp marked, aggro confirmed, party released, aborts).
//   DC_PULL_DEBUG — per-decision detail (distances, gate outcomes, phase math).
//   DC_PULL_TRACE — per-tick spam (run-in/return movement, hold-at-camp parking).
#define DC_PULL_LOG_CATEGORY "playerbots.dungeonclear.pull"
#define DC_PULL_INFO(...)  LOG_INFO(DC_PULL_LOG_CATEGORY, __VA_ARGS__)
#define DC_PULL_DEBUG(...) LOG_DEBUG(DC_PULL_LOG_CATEGORY, __VA_ARGS__)
#define DC_PULL_TRACE(...) LOG_TRACE(DC_PULL_LOG_CATEGORY, __VA_ARGS__)

class WorldObject;
class Player;
class Unit;
class Creature;
class GameObject;
class InstanceScript;
class PlayerbotAI;
class AiObjectContext;
struct DungeonBossInfo;
struct Position;

// DcPullPhase + DcPullContext now live in DcPullContext.h (included above) so the
// one owned pull-state struct is visible to both the util layer and the value
// layer without a circular include.

class DungeonClearUtil
{
public:
    // Returns the closest hostile, alive unit from `possibleTargets` that
    // sits within `range` of the bot AND within `halfAngle` radians of the
    // direction from bot to boss. Nullptr if none. Geometry-only — used as
    // the fallback when corridor computation isn't available.
    static Unit* FindBlockingTrash(Player* bot,
                                   DungeonBossInfo const& boss,
                                   float range,
                                   float halfAngle,
                                   GuidVector const& possibleTargets);


    // Returns the closest hostile, alive unit from `possibleTargets` whose
    // 2D distance to the path polyline is within its blocking band, considering
    // only the segment of the path within the first maxLookAhead yards from the
    // bot. LOS-checked. Nullptr if none. With DungeonClear.DynamicAggroRange on,
    // each candidate's band is its real aggro range (clamped to the trash
    // floor/cap); off, every candidate uses the fixed `corridorWidth`.
    static Unit* FindBlockingTrashCorridor(Player* bot,
                                           Movement::PointsArray const& corridor,
                                           float maxLookAhead,
                                           float corridorWidth,
                                           GuidVector const& possibleTargets);

    // The trash pack the advanced-pull maneuver should grab, or nullptr. Mirrors
    // the blocking-trash trigger's primary detection (corridor scan along the
    // cached long-path, falling back to the geometric cone) plus the closed-door
    // veto, so the pull aims at the same pack the normal flow would walk in to.
    // Shared by the pull trigger (decide to start) and the pull action (where to
    // run). Reads the bot's AI values via `botAI`.
    static Unit* FindPullTarget(PlayerbotAI* botAI, DungeonBossInfo const& next);

    // Returns a live spawned creature with the given entry on the bot's map, or
    // nullptr if none exists or all are dead.
    static Creature* FindLiveCreatureOnMap(Player* bot, uint32 entry);

    // Live boss creature for `entry`, resolved through the cached
    // "dungeon clear live boss" GUID (O(1) ObjectAccessor lookup) instead of
    // re-scanning the whole creature store on every call — the trigger ladder
    // and advance action ask for it several times per tick. Falls back to a
    // direct store scan when the cache was computed for a different boss (the
    // brief window just after a boss change) or the cached GUID went stale.
    // Returns nullptr when the boss isn't loaded/alive on the map.
    static Creature* GetLiveBoss(Player* bot, AiObjectContext* ctx, uint32 entry);

    // Returns true if at least one spawned creature with the given entry exists
    // on the bot's map (alive or dead). Distinguishes "missing" from "killed".
    static bool IsCreaturePresentOnMap(Player* bot, uint32 entry);








    // --- Raid / multi-tank leadership ---------------------------------------
    // Elects the single tank that drives the clear for the whole group. A party
    // has one tank, but a raid can have several (one per sub-group); without a
    // single elected leader every tank would try to drive and each sub-group's
    // members would trail their own tank instead of one raid leader. The leader
    // is the lowest-GUID alive tank BOT on `reference`'s map — a deterministic,
    // state-free choice that every member computes identically (GetFirstMember
    // walks the whole raid, not just a sub-group), so they all agree on whom to
    // follow. Real-player tanks are skipped (no PlayerbotAI to run the driving
    // AI). Returns nullptr when no tank bot is present on the map. `reference`
    // may be any group member: the issuing player, a follower, or a tank itself.
    static Player* FindLeaderTank(Player* reference);

    // True when `bot` is the elected dungeon-clear leader for its group (see
    // FindLeaderTank). Only the leader runs the driving trigger ladder and owns
    // the run's enabled/paused/progress state; every other member — non-tanks
    // AND non-leader (off-)tanks alike — follows it via the follow-tank trigger.
    static bool IsDungeonClearLeader(Player* bot);

    // True when `bot` belongs to a dungeon-clear run that is currently PAUSED —
    // either it is the elected leader and its own run is paused, or it is a
    // follower whose elected leader's run is paused. Reads the leader's
    // enabled+paused flags cross-context (same pattern as DungeonClearPartyTankValue).
    //
    // Unlike the "dungeon clear party tank" value — which deliberately resolves
    // to null while paused so followers STOP trailing the leader and revert to
    // the player — this stays true through a pause. The loot-floor filter (see
    // DungeonClearFilterLootTrigger) uses it so DC's loot policy keeps applying
    // to the WHOLE party while paused: without it, paused followers fall back to
    // the stock playerbots loot pipeline, grab below-floor junk, and keep
    // IsAnyPartyMemberLooting true — which stalls the tank.
    static bool IsInPausedDungeonClearRun(Player* bot);






    // --- Per-corpse loot give-up list ---------------------------------------
    // Prunes expired give-up entries and strips the still-live ones from the
    // stock "available loot" stack, additionally clearing "loot target" when it
    // points at a skipped GUID (can-loot reads the target, not the stack). Call
    // at the top of every DC loot-yield decision — because the advance /
    // follow-tank actions run at higher relevance than the loot pipeline, this
    // executes before stock picks its nearest target, so both the loot flags
    // AND stock's target selection skip loot the bot already gave up on. No-op
    // when the give-up list is empty (the happy path is untouched). Module-only:
    // it mutates the stock loot values, never stock code.
    static void StripSkippedLoot(PlayerbotAI* botAI);

    // Sentinel ttl meaning "never expire — give up for the rest of the run."
    // Pass as GiveUpCurrentLoot's ttl when the reason a corpse was skipped is
    // permanent: empty, below the quality floor, skinnable-only, or holding only
    // loot this bot can never take by re-looting — roll-locked / won-by-another
    // (the winner's item auto-delivers, it is never re-looted), round-robin or
    // allowed-looter sets that exclude us, or bags full with no vendor to clear
    // them mid-dungeon. None of these become takeable later, so a sticky skip
    // can never re-arm the loot yield on a backtrack.
    //
    // A real ms ttl (LOOT_SKIP_STICKY's complement) is used only for the
    // residual cases the content inspector can't pre-classify away — a camp or
    // 15s-yield timeout on loot that LOOKED takeable but didn't complete (a
    // momentary second looter on the corpse, or own loot not yet reached). There
    // the ttl just retries once rather than permanently abandon possibly-real
    // loot. Sticky entries clear only with the whole list (DisableDungeonClear).
    static constexpr uint32 LOOT_SKIP_STICKY = 0u;

    // Marks the loot the bot is currently committed to — the stock "loot
    // target" if set, else the nearest entry in the available-loot stack — as
    // given-up for `ttlMs` (or permanently when ttlMs == LOOT_SKIP_STICKY).
    // Called when a loot yield times out, or proactively when the loot is
    // un-takeable, so the bot stops re-committing to a corpse/chest it can't
    // finish. No-op when nothing of the bot's own is resolvable (e.g. a tank
    // whose yield is only IsAnyPartyMemberLooting waiting on a follower — that
    // follower gives up its own loot).
    static void GiveUpCurrentLoot(PlayerbotAI* botAI, uint32 ttlMs);

    // Fast-skips a corpse the bot has been "camped" on — standing within
    // interaction range (can-loot true) of one specific plain corpse — for
    // longer than campTimeoutMs. A normal loot transaction clears in a tick or
    // two once the bot is in range, so a long camp means the loot is
    // un-finishable for this bot (group-roll items pending a real player's roll,
    // items reserved for others, bags full). Rather than waiting out the much
    // longer loot-yield timeout on every such corpse — the "stuck on certain
    // corpses" stall — this blacklists it for giveUpTtlMs and strips it right
    // away so the loot flags drop this tick. Gathering nodes (skinning / mining
    // / herbalism) are exempt: their multi-second cast is legitimate camping.
    // Tracks the camped GUID + arrival time in the "dungeon clear loot camp *"
    // values; resets them whenever the bot isn't in range of a corpse. Returns
    // true when it skipped a corpse this call. Module-only: mutates stock loot
    // values via GiveUpCurrentLoot / StripSkippedLoot, never stock code.
    static bool MaybeGiveUpCampedLoot(PlayerbotAI* botAI, uint32 campTimeoutMs, uint32 giveUpTtlMs);

    // Decides, BEFORE the bot walks over, whether the loot it is about to commit
    // to (stock "loot target", else the nearest entry in "available loot") is
    // worth a stop — and if not, blacklists + strips it this tick so the bot
    // never detours to it (or, if already parked, leaves at once) instead of
    // discovering its emptiness by camping out the loot-yield / camp timeouts.
    // This is the proactive, event-driven counterpart to MaybeGiveUpCampedLoot:
    // creature loot is generated at kill time, so the contents are knowable
    // without opening the corpse.
    //
    // Dungeon-clear only ever stops for two kinds of loot: creature CORPSES
    // (normal kill loot) and treasure CHESTS. Every other lootable interactable
    // in the world is ignored so the bot walks straight past it instead of
    // detouring onto — and often getting stuck on — it: herbalism / mining
    // gathering nodes (chest-type gameobjects gated by a profession-skill lock),
    // skinnable-only corpses, fishing holes, levers, quest objects and loose
    // item loot. Anything that isn't a corpse-with-takeable-loot or a real chest
    // is skipped on sight.
    //
    // Drains EVERY in-range unworthy corpse in one call, not just the nearest:
    // it re-evaluates the now-nearest pickup after each skip and repeats until
    // the nearest is worth a stop (or there is none left). This keeps the tank
    // from stutter-stopping once per skipped corpse when it backtracks through a
    // field of below-floor corpses — with all of them stripped in a single tick,
    // "has available loot" never stays armed for loot the bot won't take, so the
    // advance walks straight through, held only by the party-spread gate.
    //
    // A corpse is skipped when it holds nothing this bot can take, which is two
    // overlapping cases:
    //   - un-finishable: every unlooted item is locked in a group-loot roll the
    //     bot has not won (won items are auto-delivered, never re-looted),
    //     reserved for another looter, disallowed for the bot, or the bags are
    //     full — and there is no claimable gold.
    //   - below policy: no unlooted item meets DungeonClear.LootMinQuality (an
    //     ITEM_QUALITY_* floor). Quest / quest-starter items always qualify.
    //     A floor above 0 also stops gold alone from earning a detour, so the
    //     floor actually reduces the number of stops; floor 0 (default)
    //     preserves stock "loot everything" behaviour.
    //
    // The corpse-quality check is deliberately conservative — it skips a corpse
    // only when confident nothing is takeable — because a false skip drops real
    // loot, whereas a missed skip merely falls back to the existing timeout.
    // Gameobjects are skipped unless they are genuine chests; all non-chest and
    // gathering-node interactables are dropped outright. Returns true when it
    // skipped at least one pickup this call. Module-only: mutates stock loot
    // values via GiveUpCurrentLoot / StripSkippedLoot, never stock code.
    //
    // Every reason this function skips a corpse is PERMANENT for the run (empty,
    // below the quality floor, skinnable-only, a gathering node, not a chest) —
    // none of it can become takeable later the way a pending group roll can — so
    // the skips are recorded as LOOT_SKIP_STICKY. That stops the backtrack
    // re-stutter: a field of below-floor / empty corpses stays skipped for the
    // whole run instead of re-arming the loot yield once the give-up ttl lapses.
    static bool MaybeSkipUnworthyLoot(PlayerbotAI* botAI);

    // Returns true if `creature`'s corpse holds at least one item this bot can
    // take right now (allowed, not blocked in / lost to a group roll, meeting
    // the minQuality ITEM_QUALITY_* floor — quest items always pass) or, when
    // minQuality is 0, claimable gold. Inspects the server-side creature loot,
    // filled at death. Used by MaybeSkipUnworthyLoot; returns true (don't skip)
    // for anything it cannot positively classify.
    static bool CorpseHasTakeableLoot(Player* bot, Creature* creature, uint32 minQuality);



    // Returns a per-bot "camp slot": the shared camp anchor nudged 1-2yd in a
    // deterministic, GUID-derived direction so a party holding at camp fans out
    // instead of stacking on one identical coordinate (the bot-like single-point
    // look). The offset is run through PathGenerator and the navmesh-snapped
    // endpoint is returned, so the slot is guaranteed to sit inside the zone
    // geometry; if the probe can't produce a real ground path near camp (camp
    // against a wall / on a ledge) it falls back to the exact camp position. The
    // result is stable across ticks for a given (bot, camp), so MoveTo dedups it
    // and the follower settles on one spot instead of jittering.
    static Position ComputeCampSlot(Player* bot, Position const& camp);

    // Scans `candidates` for the closest hostile alive unit whose 2D distance
    // to any segment of the supplied path polyline (within the first
    // `maxLookAhead` yards of forward travel) is within its blocking band.
    // LOS-checked. Nullptr if none. Replaces the single-segment
    // FindBlockingTrashCorridor for long routes that fan across multiple chunks.
    // With DungeonClear.DynamicAggroRange on the band is each candidate's real
    // aggro range (clamped to the trash floor/cap); off it is `corridorWidth`.
    static Unit* FindBlockingTrashOnPath(Player* bot,
                                         std::vector<PathSegment> const& segments,
                                         float maxLookAhead,
                                         float corridorWidth,
                                         GuidVector const& candidates);

    // Scans every loaded creature on the bot's map for an alive hostile that
    // (a) is not already in combat with someone else and (b) the bot can path
    // to. Returns the closest such unit, or nullptr. Used by the stalled
    // fallback to kill obstacles when no path to the boss exists.
    static Unit* FindNearestReachableHostile(Player* bot);

    // Returns the InstanceScript driving the bot's current map, or nullptr
    // if the map is not an instance map or has no script. Used by the
    // next-boss probe to consult authoritative encounter state before
    // falling back to spawn-store creature scanning.
    static InstanceScript* GetInstanceScript(Player* bot);

    // Send a structured addon message with prefix "DC" to all real players in the bot's group.
    static void SendAddonMessage(PlayerbotAI* botAI, std::string const& msg);

    // --- Event-driven status pushes -----------------------------------------
    // The companion addon used to poll `CMD\tstatus` every 2s. Instead the
    // server now recomputes status cheaply each world tick for the handful of
    // tanks actually running a clear and pushes a STATUS packet only when the
    // meaningful state changes (entered combat, pulled a boss, a boss died,
    // stalled, started looting, party recovered, …). BuildStatusPayload
    // produces the same "STATUS\t..." string DcStatusAction sends; it is shared
    // by the on-demand `dc status` action and the change-detector so the two
    // can never drift. MarkActiveTank / UnmarkActiveTank maintain the small
    // registry of clearing tanks (mirrors the follow-reaper pattern below);
    // TickStatusPushes is the throttled detector driven from the world tick.
    static std::string BuildStatusPayload(PlayerbotAI* botAI);
    // Unconditionally send the current STATUS payload and refresh the
    // change-detector's snapshot for this bot, so an explicit request never
    // provokes a duplicate push on the following tick.
    static void PushStatus(PlayerbotAI* botAI);
    static void MarkActiveTank(ObjectGuid tank);
    static void UnmarkActiveTank(ObjectGuid tank);
    static void TickStatusPushes(uint32 diff);

    // --- Orphaned follow-generator reaper -----------------------------------
    // MoveFollow is a persistent MotionMaster generator. A non-tank DC follower
    // installs one to chase the tank; its own follow-tank action tears it down
    // when the DC tank goes away (see DungeonClearFollowTankAction::Execute).
    // But that teardown only runs while the follower's PlayerbotAI is still
    // ticking. When a SELF-bot leaves bot mode (`.playerbots bot self` off),
    // playerbots `delete`s the PlayerbotAI outright — no teardown tick fires,
    // and the leftover follow generator stays installed on the now-human player,
    // gluing it to the tank with no way to self-heal (a real player has no AI to
    // clear it). MarkFollowing records every player that currently has such a
    // generator; ReapOrphanedFollows (driven each world tick from a
    // PlayerbotScript) finds any marked player still in world whose AI has gone
    // away and clears the generator, returning movement control to the player.
    static void MarkFollowing(ObjectGuid player);
    static void UnmarkFollowing(ObjectGuid player);
    static void ReapOrphanedFollows();

    // --- Dynamic pull (auto Leeroy vs Advanced) -----------------------------
    // Classifies the pull on `target`: true => use the careful Advanced pull-to-
    // camp; false => Leeroy it. Estimates how many mobs would aggro if the party
    // fought on top of the target — every mob whose own (level-scaled) aggro
    // radius + PullCombatSpread reaches the camp spot, plus one CallForHelp assist
    // hop — gated to mobs in line of sight, on the same floor, with no closed door
    // between (the far-targets scan ignores LOS). True when that estimate exceeds
    // PullDynamicMaxLeeroyMobs. Reach comes from the real creature aggro radius
    // (vs the lowest-level party member), so it self-tunes per zone/level. The pure
    // count is DungeonClearMath::EstimateAggroCount. See the Dynamic Pull plan doc.
    static bool ClassifyPullAdvanced(PlayerbotAI* botAI, Unit* target);

    // Per-tick governor for Dynamic mode (pull setting == 2). No-op for Off/On
    // (DcPullAction owns the bool there). Out of combat with no pull maneuver in
    // flight, it sizes up the next pull target (ClassifyPullAdvanced), latches the
    // verdict per target GUID (so a single approaching pack isn't re-judged every
    // tick and the party isn't churned between follow/hold), and drives `dungeon
    // clear pull mode` (+ leader daze immunity + camp seed) so the rest of the
    // existing pipeline runs the chosen maneuver. Called at the top of
    // DungeonClearPullTrigger::IsActive, which the engine evaluates before the
    // engage triggers each tick. Publishes the verdict to `dungeon clear pull
    // decision` for the addon. Leader-only (the caller has already gated on that).
    static void UpdateDynamicPullMode(PlayerbotAI* botAI, AiObjectContext* context);

    // --- Advanced pulls -----------------------------------------------------
    // True for the "holding" pull phases (Forming/Advancing/Returning) during
    // which the party stays passive and camped; false for Idle/Engage.
    static bool IsPullPhaseHolding(uint32 phase);

    // Picks the advanced-pull camp: a rally point `setback` yards BACK along the
    // already-cleared route, where the tank drags the pulled pack and the party
    // waits. Dungeon mobs have no leash, so the camp is placed a generous fixed
    // distance back for room rather than the minimum that "works". If the point
    // `setback` back is still within `safeRadius` of another pack (any live
    // hostile that is not `target` and not one of `target`'s packmates) the search
    // keeps walking back, up to `maxDrag`, until clear. Cleared route behind the
    // tank is inherently safe ground (we already killed through it). When the
    // cleared route behind is too short (e.g. the first pull near the entrance) it
    // falls back to a straight line away from the target, snapped to the navmesh.
    // Returns nullopt only when there is no usable position at all (no bot/target).
    // The chosen point's clearance is written to `clearanceOut` (FLT_MAX when no
    // other pack exists) and how far back it sits to `dragOut`, for the plan log.
    static std::optional<Position> ComputeSafeCamp(PlayerbotAI* botAI, Unit* target,
                                                   float setback, float safeRadius,
                                                   float maxDrag,
                                                   float& clearanceOut, float& dragOut);

    // Lean, target-less twin of ComputeSafeCamp for the Idle SCOUT phase: returns
    // a point `setback` back along the breadcrumb trail behind the tank (capped at
    // maxDrag), so the camp can TRAIL the moving tank while no pull is committed
    // and the party walks along behind it at a fixed standoff. No clearance test —
    // the tank just walked this ground, so it is by definition clear, and there is
    // no pack to stay clear of yet. Falls back to the farthest contiguous trail
    // point, then to the tank's own position when the trail is too short. Returns
    // nullopt only when there is no bot.
    static std::optional<Position> ComputeTrailCamp(PlayerbotAI* botAI, float setback,
                                                    float maxDrag);

    // True when the party is "set" for the tank to pull: every living, on-map,
    // non-leader BOT follower is within `setRadius` of `camp` AND currently
    // running the combat-engine "passive" strategy (so it won't break the pull).
    // Real-player members (no PlayerbotAI) are not waited on. Solo (no group)
    // returns true. Lets the Forming gate hold the tag until the party has
    // actually parked and gone passive, instead of pulling into open ground.
    static bool IsPartySetAtCamp(Player* leader, Position const& camp, float setRadius);

    // Resolves `bot`'s elected leader and, if that leader has advanced-pull mode
    // on and is in a non-Idle phase, writes the leader's pull phase and camp
    // position out and returns true. Returns false (outputs untouched) when there
    // is no leader, the run is off/paused, pull mode is off, or the phase is
    // Idle. Reads the leader's context cross-bot (same pattern as
    // IsInPausedDungeonClearRun); pass any group member.
    static bool GetLeaderPullInfo(Player* bot, uint32& phaseOut, Position& campOut);

    // True when `bot` is a non-leader follower whose elected leader is running
    // advanced-pull mode with a camp marked — in which case, in pull mode, the
    // party HOLDS at the camp and leapfrogs camp-to-camp instead of following the
    // tank (which would trail it forward into every pull). Writes the leader's
    // current camp to `campOut` and whether the party must be PASSIVE right now to
    // `passiveOut` (true only during the holding pull phases Forming/Advancing/
    // Returning; outside those the party holds at camp but stays ready to defend).
    // Returns false (outputs untouched) when there is no leader, the run is
    // off/paused, pull mode is off, `bot` is the leader, or no camp is marked yet.
    // Unlike GetLeaderPullInfo (which is true only mid-maneuver, for the passive
    // teardown), this is true throughout pull mode so the party never follows.
    static bool GetLeaderCampHold(Player* bot, Position& campOut, bool& passiveOut);

    // True when `bot` is a non-leader follower whose elected leader tank is in
    // the advanced-pull camp fight RIGHT NOW: pull phase Engage (the pack has
    // been dragged back and handed to stock combat) AND the leader is actually in
    // combat. This is the window in which a released follower must pile into the
    // pack even if the drag parked it out of the camp's line of sight (around a
    // corner) — where the stock LOS-gated target picker never acquires a target
    // and the party would otherwise stand idle and never enter the fight. Drives
    // DungeonClearAssistCamp{,Combat}Trigger. Returns false (the common case) for
    // the leader, outside pull mode, or when the leader isn't mid camp-fight.
    static bool IsLeaderCampFightActive(Player* bot);

    // True when `bot`'s elected leader is running DYNAMIC pull (pull setting == 2)
    // and is still scouting/deciding the next pack — i.e. out of combat with the
    // pull phase Idle, before it has committed to a Leeroy or an Advanced camp.
    // This is the window in which the party must hang BACK so it doesn't trail the
    // tank into an accidental aggro before the verdict is in; DungeonClearFollow
    // TankAction widens its follow distance (PullDynamicPartyLag) while it holds.
    // The instant the tank commits (enters combat, or an Advanced camp is marked),
    // this returns false and the party reverts to the tight follow / camp hold.
    // Returns false for the leader itself, outside dynamic mode, or off/paused.
    static bool IsLeaderDynamicScouting(Player* bot);

    // Point `lag` yards back along the LEADER tank's breadcrumb trail (the ground
    // the tank actually walked, which the escort spline already corridor-centered),
    // for a follower to trail to during dynamic scouting. Walking the leader's
    // trail keeps followers on the centered route instead of bee-lining a geometric
    // lag point through the raw PathGenerator, which hugs walls and ledges. Reads
    // the LEADER's crumbs cross-bot (only the tank records them) and only returns a
    // crumb `bot` can reach over a complete generated path. False if there is no
    // leader, the trail is empty, or no reachable point lies far enough back.
    static bool GetLeaderScoutTrailPoint(Player* bot, float lag, Position& out);

    // Force the leader of `bot`'s group to abandon the current pull and release
    // the party (sets the leader's pull phase to Engage). Used by the camp-safety
    // valve when a held, passive follower is taking unexpected damage. No-op if
    // there is no leader or it isn't mid-pull.
    static void AbortLeaderPull(Player* bot);

    // Grant (or revoke) the leader tank immunity to the Daze mechanic for the
    // duration of an advanced-pull session. A creature hitting a moving target
    // from behind has up to a 40% chance to Daze it (spell 1604, -50% move
    // speed) — which cripples the pull-to-camp drag-back exactly when the tank
    // most needs to retreat (it is running AWAY from the pack, so every hit
    // lands from behind). We "cheat a little" per design and make the driving
    // tank daze-proof while pull mode is on. Idempotent; also strips any Daze
    // aura already on the tank when applied. Paired with the pull-mode toggle.
    static void SetLeaderDazeImmunity(Player* leader, bool apply);

    // Keep a DRUID tank in (dire) bear form. No-op for any other class, or if
    // the bot is already shifted. Used during the advanced-pull drag-back so the
    // druid tank takes the run-home hits in bear form (extra armor/HP) instead of
    // caster form. Prefers dire bear form, falling back to bear form when dire
    // bear isn't trained. Shapeshift is instant and not movement-interrupted, so
    // it's safe to call every tick while the tank runs back to camp.
    static void EnsureTankBearForm(Player* bot);

    // --- Advanced-pull passive management -----------------------------------
    // Followers go passive (attack nothing, just hold at camp) during a pull by
    // having the mod-playerbots "passive" strategy added to their COMBAT engine.
    // ApplyFollowerPassive adds it (and sets the bot's pet passive) once, and
    // records the player in a registry; RemoveFollowerPassive reverses both. Both
    // are idempotent and only touch passive that DC itself applied — a passive a
    // player set manually is left alone. HEALERS are exempt: ApplyFollowerPassive
    // skips them so they keep the camp hold but stay free to heal the tank through
    // the drag-back (the camp-hold action yields the tick for a parked healer so
    // its heals fire). ReapStrandedPassives runs every world
    // tick (from the same PlayerbotScript as ReapOrphanedFollows) and is the
    // SINGLE authoritative teardown: it removes DC passive from any registered
    // player whose leader is no longer in a holding pull phase (pull released /
    // dc off / paused / death / leader gone), which reliably un-passives even a
    // follower that got dragged into combat — its own non-combat engine can't run
    // a teardown while the combat engine is passive-locked. The valve also aborts
    // the pull when a held, passive follower drops below the safety HP floor.
    static void ApplyFollowerPassive(Player* follower);
    static void RemoveFollowerPassive(Player* follower);
    static void ReapStrandedPassives();
};

#endif
