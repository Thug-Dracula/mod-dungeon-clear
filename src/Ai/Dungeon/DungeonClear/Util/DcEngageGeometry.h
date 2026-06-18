/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_ENGAGE_GEOMETRY_H
#define _DC_ENGAGE_GEOMETRY_H

#include <optional>

#include "MoveSplineInitArgs.h"
#include "Position.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"

class Player;
class Unit;
class Creature;
class GameObject;
class WorldObject;
class AiObjectContext;
struct DungeonBossInfo;

class DcEngageGeometry
{
public:
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

    // The distance at which an advanced pull is COMMITTED: the tank stops gliding,
    // holds, and waits for the party to set at the camp BEFORE it steps in to tag.
    // Derived from `target`'s REAL aggro radius (the exact core value from
    // Creature::GetAggroRange + both reaches + AggroRangeMargin, identical to the
    // boss handoff) so the tank Forms just OUTSIDE aggro and never face-pulls the
    // pack mid-glide — the whole point of the careful pull. Clamped to the
    // PullCommitRange floor/cap (the cap stays inside the ~35yd detection band).
    // Falls back to `staticRange` for a non-creature target or when the
    // DynamicAggroRange config is off. Cheap — no allocation, no pathfinding.
    static float PullCommitRange(Player* bot, Unit* target, float staticRange);

    // --- Room-aggro clear: skirt the boss's aggro sphere -------------------
    // SINGLE SOURCE OF TRUTH for the room-aggro "avoid sphere": the radius around
    // a flagged boss the tank/party must stay OUTSIDE while pre-clearing its room,
    // so clearing never wakes the boss. EVERY room-aggro radius derives from this
    // one value — the room-trash exclusion (DungeonClearRoomTrashValue), the skirt
    // avoid-ring (AggroSafeApproachPoint's safeRadius), the follower skirt
    // (DcLeaderSignal), and the tank's boss-engage standoff (BossEngageRange).
    // Sized off `bot`'s OWN notice distance against the boss (a low-level follower
    // differs from the tank) + both combat reaches + the aggro margin + path
    // padding: a melee unit standing within a combat reach of the nearest still-
    // clearable pack (just outside this sphere) is still kept clear of the boss's
    // real aggro. Centralising it is what ends the recurring "two radii drifted
    // apart -> dead band / aggro leak" class of bug — callers MUST use this rather
    // than re-deriving the formula. Returns 0 when either input is missing.
    static float RoomAggroSphereRadius(Player* bot, Creature* boss);

    // When clearing a room before a boss whose engage drags the WHOLE room into
    // combat (RoomAggroRegistry), the tank must reach each trash pack WITHOUT its
    // approach path crossing the boss's aggro sphere — otherwise it wakes the boss
    // mid-clear and the room piles on (the SM Cathedral / Mograine failure mode:
    // Mograine sits in the room centre, so the straight line to a pack on the far
    // side cuts right through his aggro range). The room-trash VALUE already
    // excludes mobs INSIDE the sphere, but it does nothing about the PATH to a
    // pack outside it.
    //
    // Given the boss centre (bx,by,bz) and a `safeRadius` (its aggro range + the
    // reaches + margin — the same sizing as the value's exclusion sphere), returns
    // a navmesh-snapped DETOUR waypoint to move to FIRST when the straight 2D line
    // from the bot to `target` passes within `safeRadius` of the boss: a point on
    // the safe ring, stepped from the bot's current bearing toward the target's
    // bearing (both measured from the boss), so calling it each tick walks the tank
    // AROUND the sphere on the short arc instead of through it. Returns nullopt
    // once the direct approach is clear (engage the target straight) or when no
    // walkable detour can be snapped (fall back to the direct approach rather than
    // freeze the clear).
    //
    // `orbitDir` (optional, in/out) latches the rotation direction once the skirt
    // commits to rounding the LONG way around a wall-blocked short arc, so the
    // tank rounds the whole long way (going "backward" past the boss) instead of
    // flipping back toward the target every tick and bouncing between two ring
    // points forever. Pass nullptr for a one-shot, latch-free skirt (followers).
    // Reset to 0 by this function the instant the straight approach clears.
    static std::optional<Position> AggroSafeApproachPoint(
        Player* bot, float bx, float by, float bz, float safeRadius, Unit* target,
        int8* orbitDir = nullptr);

    // The chooser behind AggroSafeApproachPoint, factored out as pure geometry so
    // it can be unit-tested without a live map: true when the straight 2D line
    // from (botX,botY) to (targetX,targetY) passes within `avoidRadius` of the
    // boss centre (bossX,bossY) — i.e. a detour is needed. A bot already inside
    // the padded sphere reads true (its endpoint is within the radius, so the
    // recovery case still demands an exit waypoint). A degenerate radius
    // (avoidRadius <= 0) reads false (nothing to skirt).
    static bool NeedsRoomAggroSkirt(float botX, float botY,
                                    float targetX, float targetY,
                                    float bossX, float bossY, float avoidRadius);

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

    // True when the tank is inside the room-clear ENVELOPE of a room-aggro boss —
    // the whole region the deliberate room-clear roams: max(room radius, skirt
    // orbit ring) + a buffer, on the boss's own floor. WIDER than IsAtBossEngage
    // (the engage standoff), which sat INSIDE the orbit ring and so dropped the
    // bot out of the room-clear window the moment it ran back onto the ring to
    // round the sphere (handing it to the boss-bound Advance — the live Jammal'an
    // failure). This is the DRIVER's window (DcTargeting::IsRoomClearActive + the
    // no-progress valve); the governor enforces a separate, narrower keep-out ring.
    // Returns false for a non-room-aggro boss (no envelope).
    static bool WithinRoomClearWindow(Player* bot, AiObjectContext* ctx,
                                      DungeonBossInfo const& boss);

    // True when the tank has actually REACHED the room-clear envelope BY PATH —
    // the same window as WithinRoomClearWindow, but measured along the navmesh
    // route to the live boss instead of straight-line. WithinRoomClearWindow (and
    // its WithinBossRangeOnFloor core) takes a same-floor straight-line shortcut,
    // so a room one WALL over reads as "in the window" even though it is a long
    // detour away by foot (Scholomance: the chamber east of Marduk & Vectus is
    // ~55yd straight-line but a 170yd+ walk around). Gating room-clear ACTIVATION
    // on that lie let the conditional room-clear event — which outranks
    // boss-travel — hijack the approach from across the map and fixate on a single
    // barely-reachable straggler. This forces the navmesh probe so room-clear only
    // engages once the tank is genuinely in the room; until then boss-travel
    // (Advance) routes it in. Returns false for a non-room-aggro boss.
    static bool TankReachedRoomByPath(Player* bot, AiObjectContext* ctx,
                                      DungeonBossInfo const& boss);

    // True when `u` is a creature that fights at RANGE — a caster or a physical
    // ranged attacker — and will therefore stand and shoot/cast from afar instead
    // of running to melee. Used by the advanced-pull camp placer to decide whether
    // the camp must break line of sight to the pack (so the rangers are forced to
    // close to the camp rather than plinking the party from across the room).
    // Detection is a cheap OR of three signals, no pathfinding:
    //   1. unit_class is a caster class (creatures map casters to MAGE; PRIEST/
    //      SHAMAN/WARLOCK are checked too in case a realm's data uses them).
    //   2. a ranged WEAPON is equipped in the virtual ranged slot — bow / gun /
    //      crossbow / wand (thrown is excluded: short range, the mob runs in).
    //   3. the creature template knows a DAMAGING spell whose max range exceeds
    //      PullRangedSpellRangeFloor (mirrors Creature::reachWithSpellAttack's
    //      effect test, minus the live mana/distance gating) — this catches a
    //      caster that the engine classed as a melee unit_class.
    // `bot` is used only to read the (per-run overridable) range-floor setting.
    // Non-creatures return false.
    static bool IsRangedAttacker(Player* bot, Unit* u);

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
    // `from` to (tx,ty), within `corridorWidth` of it, on the from/target floor
    // (Z), and projecting to the INTERIOR of that chord. Used to veto engaging a
    // boss/pack on the FAR side of a shut door (the navmesh may let the tank clip
    // through, so it would otherwise bee-line onto/through the door). Computed
    // FRESH per call on purpose — the cached "dungeon clear blocking door" value
    // (500ms) can still read empty at the moment a scan first sees the far-side
    // target, which let the tank run through, clear it, and walk back. The Z and
    // interior-projection gates keep the straight chord from grazing a door on
    // another deck or in a parallel corridor and vetoing a valid near-side pull.
    // `from` is any world object (usually the bot, but the dynamic-pull chain gate
    // passes the PACK so the door test is independent of where the tank stands).
    static bool ClosedDoorBetween(WorldObject* from, float tx, float ty, float tz,
                                  float corridorWidth = 8.0f);

    // True if a closed `GAMEOBJECT_TYPE_DOOR` sits within `radius` (2D) of the
    // point (x,y,z) and on its floor (Z band). Companion to ClosedDoorBetween:
    // that one tests a door BETWEEN two points and deliberately ignores a door
    // projecting to a chord ENDPOINT, so a point sitting squarely IN a doorway
    // slips past it. This catches that — a pull camp must never be planted in a
    // still-shut doorway (the navmesh is blind to script/event doors, so the
    // party would "stand in"/clip through a door it has not opened yet). `ref`
    // supplies the map to scan; its own position is not used.
    static bool ClosedDoorNear(WorldObject* ref, float x, float y, float z,
                               float radius = 8.0f);

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
    // candidate's level AND whose length is commensurate with the straight-line
    // distance (DC_TRASH_DETOUR_RATIO/SLACK): a mob below a ledge that "looks"
    // 15yd away but is really a 70yd ramp detour reads NOT reachable, so the
    // tank neither locks onto an unreachable above/below mob and wedges against
    // the geometry, nor wanders off on a huge detour to one it technically can
    // reach. Trash is always within a corridor/cone lookahead well under
    // PathGenerator's single-call range cap, so one direct probe is both
    // cheaper and more precise here than the strided boss pathfinder behind
    // IsReachable.
    static bool IsLevelReachable(Player* bot, Unit* u);

    // Computes the mmap path from the bot's current position to (bx, by, bz).
    // Returns true and fills `out` only when the result is PATHFIND_NORMAL —
    // a complete navigable route. Empty/incomplete/shortcut results return
    // false. The caller uses the polyline to test "is any hostile within
    // corridorWidth yards of the next maxLookAhead yards of the path."
    static bool ComputeCorridor(Player* bot,
                                float bx, float by, float bz,
                                Movement::PointsArray& out);

};

#endif  // _DC_ENGAGE_GEOMETRY_H
