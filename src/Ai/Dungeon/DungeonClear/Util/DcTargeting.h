/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_TARGETING_H
#define _DC_TARGETING_H

#include "ObjectGuid.h"
#include "MoveSplineInitArgs.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"

class Player;
class Unit;
class Creature;
class GameObject;
class InstanceScript;
class AiObjectContext;
class PlayerbotAI;
struct DungeonBossInfo;

class DcTargeting
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

    // The trash pack the advanced-pull maneuver should grab, or nullptr. Mirrors
    // the blocking-trash trigger's primary detection (corridor scan along the
    // cached long-path, falling back to the geometric cone) plus the closed-door
    // veto, so the pull aims at the same pack the normal flow would walk in to.
    // Shared by the pull trigger (decide to start) and the pull action (where to
    // run). Reads the bot's AI values via `botAI`.
    static Unit* FindPullTarget(PlayerbotAI* botAI, DungeonBossInfo const& next);

    // Live pull-target resolve through the cached/sticky "dungeon clear pull
    // target" GUID (see DungeonClearPullTargetValue) instead of re-running the
    // corridor scan at every call site — the pull pipeline asks for it 5+ times
    // per out-of-combat tick. Re-resolves the GUID via ObjectAccessor every call
    // (the position stays live, no pointer outlives the tick) and forces one
    // recompute when the cached GUID went stale within the interval (the pack
    // died), so a commit never aims at a corpse. Nullptr when no pull target.
    static Unit* GetPullTarget(PlayerbotAI* botAI);

    // True when `entry` is one of the run's encounter bosses (membership test
    // against the "dungeon bosses" value). Linear scan — the list is ≤ ~20
    // entries; build a set snapshot alongside the vector if profiling ever
    // cares. Keeps bosses out of the pull pipeline: the dedicated at-boss path
    // (engage-range gate, anchor checks, door veto, party-ready gate) owns
    // them, and scripted bosses misbehave when camp-dragged.
    static bool IsDungeonBossEntry(AiObjectContext* ctx, uint32 entry);

    // Validity predicate for KEEPING the sticky pull target between scans:
    // alive, hostile, not a dungeon boss, not the pull context's abort target,
    // within the scan look-ahead plus a slack band, level-reachable, and no
    // closed door between.
    // Deliberately does NOT require it to still be the closest blocker or
    // inside the scan corridor — packs drift while patrolling, and re-running
    // corridor membership is exactly the target-flapping instability the sticky
    // latch removes.
    static bool IsStickyPullTargetValid(Player* bot, AiObjectContext* ctx, Unit* u);

    // Scans every loaded creature on the bot's map for an alive hostile that
    // (a) is not already in combat with someone else and (b) the bot can path
    // to. Returns the closest such unit, or nullptr. Used by the stalled
    // fallback to kill obstacles when no path to the boss exists.
    static Unit* FindNearestReachableHostile(Player* bot);

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

    // --- Event-summoned bosses (e.g. RFD's gong -> Tuten'kash) ------------

    // True when `bossEntry` is the credit boss of an un-retired CONDITIONAL event
    // on the bot's map (DungeonEventRegistry, matched via panelGatesBossEntry) —
    // i.e. a boss that an event must SUMMON before it exists. Such a boss is
    // legitimately absent from the creature store until the event runs, so the
    // approach must not "not-spawned" stall on it.
    static bool HasPendingSummonEvent(Player* bot, AiObjectContext* ctx, uint32 bossEntry);

    // True when the leader is parked at a pending-summon boss's anchor: the boss
    // is the next target, has a pending summon event, is not yet live, and the
    // tank is within the event hold radius of the anchor. While this holds, the
    // dynamic-pull pipeline stands down so the tank stays put to run the event
    // (ring the gong, fight the wave, ring again) instead of wandering off to
    // pull distant trash and never returning. Reads false the instant the boss
    // goes live (normal engage takes over) or the tank leaves the hold radius.
    static bool IsHoldingForSummonEvent(Player* bot, AiObjectContext* ctx,
                                        DungeonBossInfo const& next);

    // Returns the InstanceScript driving the bot's current map, or nullptr
    // if the map is not an instance map or has no script. Used by the
    // next-boss probe to consult authoritative encounter state before
    // falling back to spawn-store creature scanning.
    static InstanceScript* GetInstanceScript(Player* bot);

    // --- Room-wide-aggro pre-clear (RoomAggroRegistry) --------------------

    // True when the next boss is a flagged room-aggro boss AND the tank is at
    // its engage range AND room trash still remains to clear (the "dungeon clear
    // room trash remaining" value is non-empty). This is the predicate that both
    // holds the boss pull and routes the trash clear: while it is true the boss
    // gate stands down and the pull pipeline / room-clear action work the room.
    static bool IsRoomClearActive(Player* bot, AiObjectContext* ctx);

    // The nearest remaining room-trash unit (from "dungeon clear room trash
    // remaining"), or nullptr. Nearest-first so the tank clears the room from
    // its edge inward and reaches the boss's own aggro sphere last, minimising
    // the chance of waking the boss while clearing.
    static Unit* NearestRoomTrash(Player* bot, AiObjectContext* ctx);

    // The nearest reachable, attackable hostile within `radius` (2D) of the point
    // (px,py,pz) and within `zBand` vertically — or nullptr. Backs the ClearRadius
    // event step (a POINT-anchored room pre-clear, e.g. Sunken Temple's central
    // circle before Jammal'an): position-based, NOT entry-based, so it clears
    // whatever patrols the area regardless of entry. Excludes encounter and
    // room-aggro bosses, the unreachable, and door-blocked units. Nearest-to-the-
    // BOT so the tank works inward from where it stands. zBand keeps a multi-level
    // chamber's balconies (above) and pit (below) out of a floor-level clear.
    static Unit* NearestHostileNearPoint(Player* bot, AiObjectContext* ctx,
                                         float px, float py, float pz,
                                         float radius, float zBand = 20.0f);

};

#endif  // _DC_TARGETING_H
