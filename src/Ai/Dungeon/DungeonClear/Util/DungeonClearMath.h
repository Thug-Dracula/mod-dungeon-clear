/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARMATH_H
#define _PLAYERBOT_DUNGEONCLEARMATH_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Position.h"

namespace DungeonClearMath
{
    // Sentinel "no rejoin crumb" returned by FindTrailRejoin.
    inline constexpr std::size_t TrailRejoinNone = static_cast<std::size_t>(-1);

    // One forward hostile for the Dynamic-pull aggro estimate. `chainEligible` is
    // pre-resolved by the caller from game state: true only when this mob is
    // navmesh-reachable from the pull target with a clear line of sight and no
    // closed door between (the far-targets scan ignores LOS, so this gate keeps a
    // mob through a wall / a floor away / behind a door from counting as one that
    // would aggro). It is ignored for mobs that share the target's own pack. `z`
    // carries the mob's world height so the estimate can reject mobs on another
    // floor (a ledge/ramp directly above or below) instead of counting them by
    // plan-view distance alone — see `zTolerance` on EstimateAggroCount.
    //
    // `aggroReach` is this mob's real aggro radius (Creature::GetAggroRange +
    // GetCombatReach, level-diff scaled — see DungeonClearUtil::ClassifyPull-
    // Advanced) measured against the squishiest party member. It is the distance
    // at which the mob proximity-aggros the party fighting at the camp spot, so it
    // self-tunes per zone/level instead of a single hand-tuned chain radius.
    //
    // `packId` is an OPTIONAL engine-pack identity (0 = none) the caller resolves
    // from what actually pulls together: a creature formation group, or a
    // creature_linked_respawn link. Mobs sharing a non-zero `packId` pull as one
    // atomic unit regardless of spacing or height (you cannot pull half a
    // formation), so a counted member drags its whole pack into the estimate.
    // `patroller` marks a DB-authored waypoint mover (GetDefaultMovementType() ==
    // WAYPOINT_MOTION_TYPE). A human tank waits a patrol out instead of committing
    // the heavier pull for a pack that would be a clean small Leeroy moments later;
    // the patrol-wait gate re-runs the estimate with lone patrollers excluded (see
    // `excludeLonePatrollers` on EstimateAggroCount) to detect that case. Random
    // wanderers are deliberately NOT patrollers — their return is unpredictable.
    struct DynPullMob
    {
        float         x;
        float         y;
        float         z;
        bool          chainEligible;
        std::uint32_t packId = 0;
        float         aggroReach = 0.0f;
        bool          patroller = false;
    };

    // Pure Dynamic-pull estimate: how many mobs aggro if the party Leeroys on top
    // of `mobs[targetIdx]`? The camp spot is the target's position (where a Leeroy
    // fight happens). Returns the estimated body count; the caller compares it to
    // its Leeroy ceiling (count > ceiling => Advanced pull, else Leeroy).
    //   - Seed = the target, its atomic pack (shared non-zero `packId`), and every
    //     other mob that proximity-aggros the camp: within `aggroReach + combat-
    //     Spread` (2D), on the same level (`zTolerance`), and `chainEligible`.
    //     `combatSpread` widens the camp from a point to a disc to model players
    //     drifting to flank/kite during the fight.
    //   - One assist hop: a `chainEligible` mob within `assistRadius` (2D, same
    //     level) of a SEED mob joins via CallForHelp. Proximity aggro from a fixed
    //     camp does not chain, so assisted mobs do NOT seed further proximity or
    //     assist — exactly one ring.
    //   - Formation closure: any pack touched by the set is counted in full.
    // `zTolerance` keeps the estimate honest in multi-level rooms: WotLK inter-
    // floor gaps exceed it, so a mob a ramp above/below never counts. Separated
    // from the game-state resolution in DcPullPlanner::ClassifyPullAdvanced so
    // the logic is unit-testable.
    // `excludeLonePatrollers` (patrol-wait gate): when true, a mob marked
    // `patroller` that is NOT part of an atomic pack (`packId == 0`) is treated as
    // not `chainEligible` — it neither seeds proximity nor assists nor is assisted
    // — modelling "if we let this patrol pass". Formation/linked patrollers
    // (`packId != 0`) are unaffected: you cannot wait out half a formation.
    // `countedOut` (optional) receives the indices of every mob the estimate
    // counted, for diagnostic logging at the call site.
    std::uint32_t EstimateAggroCount(std::vector<DynPullMob> const& mobs,
                                     std::size_t targetIdx, float combatSpread,
                                     float assistRadius, float zTolerance,
                                     bool excludeLonePatrollers = false,
                                     std::vector<std::size_t>* countedOut = nullptr);

    // Pull CC-assist grace gate (pure). Decides whether a CC-impaired drag-back
    // should be ABORTED so the party drops passive and piles in to help the tank.
    // `impaired` is the caller's verdict that the leader tank is currently under a
    // pull-ruining control effect (stun / fear / confuse / root / heavy slow).
    // `ccSince` is the timestamp the CURRENT continuous impairment began (0 = not
    // impaired right now). Each tick: while impaired, arm/keep the latch and abort
    // once it has persisted for `graceMs`; while clear, disarm it. A brief micro-CC
    // that clears within the grace therefore never throws the pull away on a
    // flicker, while sustained CC (the pull is failing) releases the party. With
    // `graceMs` == 0 the very first impaired tick aborts. Returns true to abort and
    // writes the updated latch to `ccSinceOut` (which the caller persists in the
    // pull context). Separated from the Unit-state read at the call site so the
    // timing logic is unit-testable.
    bool ShouldAbortPullForCc(bool impaired, std::uint32_t ccSince,
                              std::uint32_t now, std::uint32_t graceMs,
                              std::uint32_t& ccSinceOut);

    // Dynamic-verdict drop grace gate (pure). A standing Leeroy/Advanced verdict
    // must survive a TRANSIENT no-target read (door veto flicker, long-path cache
    // mid-rebuild, far-targets poll boundary): dropping it instantly flips the
    // pull-mode bool, releasing the camp hold and stripping daze immunity for a
    // single bad tick, then re-deriving everything — the party lurch. `targetPresent`
    // is the caller's verdict that the pull target resolved this tick. `lostSince`
    // is the timestamp the target first resolved null while the verdict was
    // standing (0 = present). Each tick: while lost, arm/keep the latch and drop
    // once it has persisted for `graceMs`; while present, disarm it. Mirrors
    // ShouldAbortPullForCc's latch/clear contract (including the now==0 corner
    // and graceMs==0 => drop on the first lost tick). Returns true to drop the
    // verdict and writes the updated latch to `lostSinceOut` (persisted in
    // DcPullContext::targetLostSince by the caller).
    bool ShouldDropPullVerdict(bool targetPresent, std::uint32_t lostSince,
                               std::uint32_t now, std::uint32_t graceMs,
                               std::uint32_t& lostSinceOut);

    // Leeroy roll-in gate (pure). True when the party's scout-lag hold should
    // release because the tank is committing to a Leeroy on the verdicted pack:
    // the standing decision is Leeroy (1) and the tank is within
    // `commitRange + lead` (2D) of the live target. `decision` 0 (none, still
    // scouting) and 2 (Advanced — the camp machinery owns the party) never roll
    // in, nor does a dead/unresolvable target. The game-state resolution
    // (decision, live target, distances) stays in
    // DcLeaderSignal::IsLeaderDynamicScouting; this carries only the decision
    // logic so it is unit-testable.
    bool ShouldRollInForLeeroy(std::uint32_t decision, bool targetAlive,
                               float tankToTarget2d, float commitRange, float lead);

    // Patrol-wait gate (pure). A pull is "patrol-contended" when the ONLY thing
    // pushing the aggro estimate over the Leeroy ceiling is one or more lone
    // patrollers: `fullCount > ceiling` but `reducedCount <= ceiling` (the reduced
    // pass excluded them — see EstimateAggroCount's excludeLonePatrollers). For
    // such a pack a human waits the patrol out rather than committing the heavier
    // Advanced maneuver. Returns true to KEEP WAITING (hold the pull), false to
    // proceed (commit the standing verdict). Contended: arm/keep the `waitSince`
    // latch and wait until `waitMs` has elapsed, then proceed with the heavy
    // verdict (don't stall the run for a stationary/slow patrol) — the latch stays
    // armed past the timeout so the wait does not re-fire on the same contention.
    // Not contended (patrol left chain range, or never the cause): clear the latch
    // and proceed at once. `waitMs` == 0 proceeds immediately. Mirrors
    // ShouldAbortPullForCc's by-reference latch/clear contract. The game-state read
    // (the two estimates, the live target/commit distance that decides WHEN to arm)
    // stays in DcPullPlanner::UpdateDynamicPullMode.
    bool ShouldWaitForPatrol(std::uint32_t fullCount, std::uint32_t reducedCount,
                             std::uint32_t ceiling, std::uint32_t waitSince,
                             std::uint32_t now, std::uint32_t waitMs,
                             std::uint32_t& waitSinceOut);

    // Turn-and-plant gate (pure). A human tank dragging a pack back to camp turns
    // and fights the moment the pack is glued to it, rather than sprinting the
    // whole leg back-turned (the single biggest visual bot-tell, and why the
    // daze-immunity cheat exists). True when the drag-back should stop early and
    // the tank turn to fight: the pack is gathered (EVERY live attacker distance
    // <= `glueRadius`) for >= `glueTicksNeeded` consecutive maneuver ticks, this
    // is not an LOS-break pull (`losPull` — those must reach the corner), there is
    // something chasing (empty `attackerDists` => evade/fizzle, never a plant),
    // and at least the first HALF of the return leg is covered
    // (`distToCamp <= legStartDist / 2`, keeping the neighbour-pack clearance the
    // camp was measured for). `plantTicks` is the per-pull debounce latch: it is
    // incremented while the gather condition holds and reset to 0 the moment it
    // breaks, mirroring ShouldAbortPullForCc's by-reference latch contract — so a
    // single-tick noise spike can never trip an early plant. The game-state read
    // (attacker distances, leg progress) stays in DungeonClearPullManeuverAction;
    // this carries only the decision so it is unit-testable.
    bool ShouldPlantEarly(std::vector<float> const& attackerDists, float glueRadius,
                          std::uint32_t glueTicksNeeded, bool losPull,
                          float distToCamp, float legStartDist,
                          std::uint32_t& plantTicks);

    // Threat-lead follower-release gate (pure). After the leader tank enters
    // combat a real group gives it a beat to gather and establish AoE threat
    // before DPS pile in; this holds a follower's fight assist for `leadMs` after
    // the leader's CURRENT combat began (`combatSinceMs`; 0 = leader not in
    // combat). Healers release immediately (`isHealer` — a withheld heal is a
    // wipe and heals don't rip threat the way DPS openers do). DPS release once
    // the lead has elapsed, with two bypasses: the tank's HP below `panicHpPct`
    // (it is LOSING the fight — pile in; <= 0 disables the bypass) and `leadMs`
    // == 0 (feature off). The game-state read (leader combat stamp, healer role,
    // tank HP) stays in DcLeaderSignal::IsLeaderFightAssistWanted.
    bool ShouldReleaseFollower(bool isHealer, std::uint32_t combatSinceMs,
                               std::uint32_t now, std::uint32_t leadMs,
                               float tankHealthPct, float panicHpPct);

    // Squared 2D distance from point P to segment (A,B).
    float DistSqToSegment2D(float px, float py,
                            float ax, float ay,
                            float bx, float by);

    // True if the 2D segment (A,B) intersects the axis-aligned box
    // [minX,maxX] x [minY,maxY]. Liang-Barsky slab clip: returns true even
    // when BOTH endpoints lie outside the box but the segment passes through
    // it (the case that matters for a thin door panel a path step straddles).
    bool SegmentIntersectsAABB2D(float ax, float ay, float bx, float by,
                                 float minX, float minY,
                                 float maxX, float maxY);

    // Index of the LATEST crumb within `rejoinRadius` (3D) of `cur`, or
    // TrailRejoinNone if none qualifies. Used by the breadcrumb recorder: on a
    // >kJump discontinuity (a drag-back / drop-down), rather than wiping the
    // whole trail, rejoin at the most recent crumb near where the bot now stands
    // and truncate everything ahead of it. Latest-wins so a trail that loops near
    // itself rejoins at the most recent pass, keeping the walked-distance
    // semantics the camp walk-back relies on intact. 3D on purpose — a crumb
    // directly above/below (different floor) must not count as a rejoin.
    std::size_t FindTrailRejoin(std::vector<Position> const& crumbs,
                                Position const& cur, float rejoinRadius);
}

#endif
