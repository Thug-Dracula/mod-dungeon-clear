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
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"

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

class Player;
class Unit;
class Creature;
class GameObject;
class InstanceScript;
class PlayerbotAI;
class AiObjectContext;
struct DungeonBossInfo;
struct Position;

// Advanced-pull (LOS pull-to-camp) sub-phase. Stored in the leader's
// "dungeon clear pull phase" value as a uint32 and read cross-context by
// followers. Forming/Advancing/Returning are the "holding" phases that keep the
// party passive at camp; Idle and Engage release them. See
// DungeonClearPullPhaseValue and DungeonClearUtil::IsPullPhaseHolding.
enum class DcPullPhase : uint32
{
    Idle      = 0,
    Forming   = 1,
    Advancing = 2,
    Returning = 3,
    Engage    = 4,
};

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

    // Computes the mmap path from the bot's current position to (bx, by, bz).
    // Returns true and fills `out` only when the result is PATHFIND_NORMAL —
    // a complete navigable route. Empty/incomplete/shortcut results return
    // false. The caller uses the polyline to test "is any hostile within
    // corridorWidth yards of the next maxLookAhead yards of the path."
    static bool ComputeCorridor(Player* bot,
                                float bx, float by, float bz,
                                Movement::PointsArray& out);

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

    // --- Dynamic aggro range ------------------------------------------------
    // The distance at which `u` would aggro `bot`. For a Creature this is the
    // core's own GetAggroRange (detection range adjusted by level difference,
    // clamped 5-45yd by the core), so the value already reflects giant elites
    // (large detection) and neutral/low-level ambient mobs (small detection)
    // instead of a single hardcoded band. Clamped to [floorYd, capYd] so a
    // pathological value can never blow out the caller's scan/engage geometry.
    // Non-creatures (and a disabled DungeonClear.DynamicAggroRange config)
    // return `fallback`. Cheap — no allocation, no pathfinding.
    static float AggroRangeOf(Player* bot, Unit* u, float fallback,
                              float floorYd, float capYd);

    // The distance at which the tank should consider itself "at the boss" and
    // hand off from the smooth long-path/direct-pursuit glide to the decisive
    // engage pull. Derived from the LIVE boss's actual aggro range plus the
    // tank's reach and a margin, so a small-aggro boss yields a short range
    // (the smooth glide carries the tank most of the way in before the pull,
    // killing the old stutter-creep) while a large-aggro elite yields a longer
    // one (the tank commits to the pull right as it would aggro anyway). Falls
    // back to `staticRange` when the boss isn't loaded yet or the dynamic-aggro
    // config is off. The trigger ladder and the advance action MUST both read
    // this so they agree on "are we at the boss".
    static float BossEngageRange(Player* bot, AiObjectContext* ctx,
                                 DungeonBossInfo const& boss, float staticRange);

    // True when the tank is close enough AND on a navigable level with the boss
    // to hand off from route-following to the decisive engage pull. The 3D
    // distance alone (BossEngageRange) treats a boss one floor up/down as
    // "arrived" while the tank is still running underneath it to reach the ramp
    // — which parks the tank under the boss forever ("boss is close", never
    // pulls). This adds the trash scan's level-reachability probe so the handoff
    // is deferred until the route lifts the tank onto the boss's own floor. The
    // at-boss trigger, the blocking-trash trigger, and the advance action MUST
    // all gate on this so they agree on "are we at the boss".
    static bool IsAtBossEngage(Player* bot, AiObjectContext* ctx,
                               DungeonBossInfo const& boss, float staticRange);

    // True when `go` is a navigation-blocking door currently in its CLOSED
    // (corridor-blocking) visual state. Encapsulates the startOpen-inverted
    // GOState test shared by the blocking-door scan and the door-reopened
    // auto-resume trigger: the GOState->open/closed mapping is inverted by the
    // door.startOpen template flag, so "closed" is `GO_STATE_READY xor
    // startOpen` (mirrors the client). Non-door GOs, doors flagged
    // ignoredByPathing (authored always-passable), and null/out-of-world GOs
    // all read NOT closed.
    static bool IsDoorClosed(GameObject const* go);

    // True if a closed `GAMEOBJECT_TYPE_DOOR` sits on the straight 2D line from
    // the bot to (tx,ty), within `corridorWidth` of it, on the bot/target floor
    // (Z), and projecting to the INTERIOR of that chord. Used to veto engaging a
    // boss/pack on the FAR side of a shut door (the navmesh may let the tank clip
    // through, so it would otherwise bee-line onto/through the door). Computed
    // FRESH per call on purpose — the cached "dungeon clear blocking door" value
    // (500ms) can still read empty at the moment a scan first sees the far-side
    // target, which let the tank run through, clear it, and walk back. The Z and
    // interior-projection gates keep the straight chord from grazing a door on
    // another deck or in a parallel corridor and vetoing a valid near-side pull.
    static bool ClosedDoorBetween(Player* bot, float tx, float ty, float tz,
                                  float corridorWidth = 8.0f);

    // Distance TRAVELLED ALONG the long-path (from the bot) to where the route
    // first comes within the door band of the door at (doorX,doorY), or FLT_MAX
    // if it never does within `maxLookAhead`. The door-blocked handler parks the
    // tank a short stand-off before this so it stops on the NEAR side of the
    // doorway. Measured along the path on purpose: GetExactDist to a door's GO
    // origin (hinge/jamb) is unreliable — the origin can sit past the doorway
    // gap, so "within Nyd of the origin" can require walking THROUGH the gap.
    // Targets ONE specific door (the blocking one the caller already resolved):
    // considering every nearby door returned whichever the path grazed first,
    // often an off-route side door reached via a long detour, masking the real
    // blocker until the tank was already on top of it.
    static float DistAlongPathToClosedDoor(
        Player* bot, ChunkedPathfinder::Result const& path,
        float doorX, float doorY, float doorZ, float maxLookAhead);

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

    // The HP/mana percentages the between-pulls rest gate (IsPartyReady) holds
    // for. These default to mod-playerbots' own drink/eat stop thresholds
    // (AiPlayerbot.AlmostFullHealth / AiPlayerbot.HighMana): a stock bot only eats
    // back up to AlmostFullHealth and drinks back up to HighMana, then stops, so
    // we clamp the gate to those targets to keep it reachable by resting alone.
    //
    // When the run sets DungeonClear.RestHealthPct / RestManaPct (> 0) the group
    // overrides those targets for this run: the gate uses the override directly
    // (the matching DungeonClearNeeds{Eat,Drink} triggers make bots eat/drink up
    // to it, so even a target above the playerbots stop value is reachable). The
    // `bot` resolves the run owner for the override lookup; pass any member.
    // See the README's "mod-playerbots interaction" section.
    static float RestMinHpPct(Player* bot = nullptr);
    static float RestMinMpPct(Player* bot = nullptr);

    // Returns true when the party has caught up and recovered enough to pull again:
    //  - every living party member on the bot's map has HP% >= minHpPct,
    //  - every living mana-using party member has mana% >= minMpPct,
    //  - every living member is within maxSpread yards of the bot.
    // Dead members are not blocking — the party-died trigger handles them.
    // Callers pass RestMinHpPct()/RestMinMpPct() for the recovery thresholds.
    static bool IsPartyReady(Player* bot, float minHpPct, float minMpPct, float maxSpread);

    // Returns true if any LIVING bot party member on the bot's map (excluding
    // `bot` itself) currently has a corpse it intends to loot — in any phase,
    // walking in (has available loot) or within reach (can loot). Reads each
    // member's own loot values cross-context (same pattern as
    // DungeonClearPartyTankValue); real players (no PlayerbotAI) are skipped
    // since we neither drive nor wait on their looting. Lets the dungeon-clear
    // tank hold its advance after a pull until the whole party has finished
    // looting; the caller bounds the wait with a commit-timeout.
    static bool IsAnyPartyMemberLooting(Player* bot);

    // Builds a short, human-readable account of who the tank is waiting on to
    // become pull-ready, using the SAME thresholds IsPartyReady is called with
    // (so the description always matches the gate that actually holds the
    // advance). Lists each living on-map member that is too far, low on health,
    // or low on mana, with the limiting reason — e.g. "Bob (low HP), Alice (out
    // of range)". Caps the list so the addon line stays short; extra members
    // collapse to "+N more". Returns "" when the party is ready (nobody to wait
    // on). Used by DcStatusAction to fill the addon "resting" detail.
    static std::string DescribePartyNotReady(Player* bot,
                                             float minHpPct, float minMpPct,
                                             float maxSpread);

    // Names the living bot party members currently looting (walking to or
    // standing on a corpse), comma-joined and capped like DescribePartyNotReady.
    // Returns "" when only the tank itself is looting / nobody is. Used to fill
    // the addon "looting" detail so the player can see who is holding up the
    // advance.
    static std::string DescribePartyLooting(Player* bot);

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

    // Returns true if at least one navigable chunk of the path to (x, y, z)
    // exists. Delegates to ChunkedPathfinder::IsReachable, which is more
    // permissive than the legacy strict-PATHFIND_NORMAL check — partial
    // paths are acceptable because Advance now chunks long routes hop by hop.
    // The strict per-tick "actually making forward progress" check lives in
    // the position-based stuck detector inside DungeonClearAdvanceAction.
    static bool IsReachable(Player* bot, float x, float y, float z);

    // Vertical-aware reachability gate for trash/proximity target selection.
    // A candidate within DC_Z_LEVEL_TOLERANCE of the bot's Z is treated as
    // same-level and accepted with no pathfinder probe (the cheap path for
    // flat corridors). Beyond that tolerance the candidate is on a different
    // level — a balcony above, a pit below — where line-of-sight through
    // railings or floor gaps routinely reports a target the bot cannot path
    // to. For those we run a single short-range PathGenerator probe and accept
    // only a PATHFIND_NORMAL route whose endpoint actually lands on the
    // candidate's level. This stops the tank from locking onto an above/below
    // mob it can never reach and wedging against the geometry. Trash is always
    // within a corridor/cone lookahead well under PathGenerator's single-call
    // range cap, so one direct probe is both cheaper and more precise here than
    // the strided boss pathfinder behind IsReachable.
    static bool IsLevelReachable(Player* bot, Unit* u);

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

    // --- Advanced-pull passive management -----------------------------------
    // Followers go passive (attack nothing, just hold at camp) during a pull by
    // having the mod-playerbots "passive" strategy added to their COMBAT engine.
    // ApplyFollowerPassive adds it (and sets the bot's pet passive) once, and
    // records the player in a registry; RemoveFollowerPassive reverses both. Both
    // are idempotent and only touch passive that DC itself applied — a passive a
    // player set manually is left alone. ReapStrandedPassives runs every world
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
