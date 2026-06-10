/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARMATH_H
#define _PLAYERBOT_DUNGEONCLEARMATH_H

#include <cstdint>
#include <vector>

namespace DungeonClearMath
{
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
    struct DynPullMob
    {
        float         x;
        float         y;
        float         z;
        bool          chainEligible;
        std::uint32_t packId = 0;
        float         aggroReach = 0.0f;
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
    // `countedOut` (optional) receives the indices of every mob the estimate
    // counted, for diagnostic logging at the call site.
    std::uint32_t EstimateAggroCount(std::vector<DynPullMob> const& mobs,
                                     std::size_t targetIdx, float combatSpread,
                                     float assistRadius, float zTolerance,
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
}

#endif
