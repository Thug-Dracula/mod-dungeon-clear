/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DUNGEON_CLEAR_TUNING_H
#define _DUNGEON_CLEAR_TUNING_H

#include <cmath>
#include <cstdint>

using uint32 = std::uint32_t;  // matches the core's Define.h typedef; this
                               // header stays core-include-free (tests build it
                               // standalone), so alias locally instead.

// Single source of truth for the DungeonClear tuning constants that are SHARED
// across translation units. Historically several of these were defined twice
// (once in Trigger/DungeonClearTriggers.cpp, once in
// Action/DungeonClearActions.cpp) with comments warning they "must stay in
// sync" — kept aligned only by hand. Centralizing them here makes drift
// impossible: one definition, every site includes it.
//
// Phase 1 of the constant-centralization refactor only houses the previously
// DUPLICATED constants. File-local (single-TU) constants stay where they are;
// Bucket-A consolidation and Bucket-B promotion to DcSettings are later phases.
//
// NOTE: namespace-scope constexpr has internal linkage, so each including TU
// gets its own internal copy seeded from this one definition — no ODR issue and
// no linker conflict. The DC_ names are kept at global scope (as they were
// effectively file-global via the old anonymous-namespace defs) so call sites
// stay unqualified and unchanged.

// Asymmetric ranges so a trash pack sitting just outside the boss room gets
// engaged before the at-boss trigger fires. 22yd is just outside most level-80
// elite aggro radii (~18-20yd), giving room to position before melee. The
// trigger uses this to decide "at the boss"; the action uses it for the same
// hand-off, so they must agree.
constexpr float DC_ENGAGE_RANGE = 22.0f;

// Extra standoff added OUTSIDE a room-aggro boss's skirt sphere when computing
// its (uncapped) boss-engage range — see DcEngageGeometry::BossEngageRange. The
// engage hand-off for a room-aggro boss must trip while the tank is still clear
// of the sphere, with a few yards of slack so a per-tick approach glide that
// overshoots the hand-off distance still stops before crossing the sphere edge
// (and waking the boss mid-clear). Small: the sphere already carries the aggro
// margin + path padding beyond the boss's real wake distance.
constexpr float DC_ROOM_AGGRO_STANDOFF_BUFFER = 4.0f;
// Ordering invariant: the tank's hold/clear standoff is the avoid sphere PLUS
// this buffer, so it must be strictly outside the sphere. Encoded at compile time
// so a future edit that zeroes/negates the buffer (re-introducing the "engage
// hand-off lands inside the sphere" regression) fails the build instead of
// silently waking the boss mid-clear.
static_assert(DC_ROOM_AGGRO_STANDOFF_BUFFER > 0.0f,
              "room-aggro standoff must sit OUTSIDE the avoid sphere");

// Cone scan for "blocking trash" — the geometric fallback the trigger uses and
// the action falls back to when the corridor path scan can't run (boss off-mesh,
// etc.). 35yd catches packs one tick-cycle earlier than the engage range. Both
// TUs feed it to DcTargeting::FindBlockingTrash, so it is one constant
// despite the old per-context names (DC_ENGAGE_CONE_* / DC_TRASH_CONE_*).
constexpr float DC_TRASH_CONE_RANGE = 35.0f;
// pi/3 spelled as a literal rather than M_PI: MSVC only defines M_PI when
// _USE_MATH_DEFINES is set before <cmath>, so the macro is absent on Windows
// (broke the build with "M_PI: undeclared identifier"). The literal is
// portable and keeps this header constant-expression and core-include-free.
constexpr float DC_TRASH_CONE_HALF_ANGLE = 1.0471975512f;  // pi/3 == 60°

// When true, evaluate "blocking trash" via the bot's actual mmap path polyline
// instead of the geometric cone. Catches packs around corners and avoids "pack
// on the other side of a wall" false positives. Falls back to the cone scan when
// path computation fails. Both TUs must agree or one path scans while the other
// does not.
constexpr bool  DC_USE_CORRIDOR_SCAN = true;
constexpr float DC_CORRIDOR_LOOKAHEAD = 35.0f;
// Half-width of the path "blocking trash" band. Widened from 8 to 18 so it
// roughly matches level-80 elite aggro radius: a pack sitting a few yards off the
// route line still aggros as the tank passes, so it must count as blocking trash.
// With the old 8yd band such a pack was never a candidate, so the selector picked
// an on-line mob farther ahead and the tank ran straight through the side pack to
// reach it. Trigger and action share the FindBlockingTrashOnPath band, so this
// must be one value.
constexpr float DC_CORRIDOR_WIDTH = 18.0f;

// Advanced-pull commit range: a pull is only STARTED once its target pack is
// within this distance, so the camp (stamped at the tank's spot) stays close
// enough that the run-in reaches the pack and the drag-back is short. The
// corridor scan sees packs out to ~35yd, so without this the camp lands in the
// run-in's overshoot dead band and every pull after the first whiffs. The pull
// trigger and the pull action must agree on when a pull starts.
constexpr float DC_PULL_START_RANGE = 26.0f;

// How long a camp write by the pull machinery (prospective publish, commit,
// dynamic seed, unplanned-aggro fresh camp) counts as "fresh". While fresh, the
// pull action owns the camp and Advance's scout camp-trailing stands down; once
// it goes stale (the pull trigger is loot/ready-gated, or no pull is running)
// Advance trails the camp behind the moving tank every tick. Replaces the old
// "is a pack in pull-scan range" ownership test, whose conditions were weaker
// than the ones the pull action actually needs to run — any tick the two
// disagreed, NOBODY moved the camp, and the spread gate (camp-anchored in pull
// mode) kept passing while the tank glided away from the party.
constexpr uint32 DC_CAMP_PUBLISH_FRESH_MS = 1000;

// The max party-spread default lives in DcSettingsRegistry ("PartyMaxSpread");
// the trigger, the advance gate, and the status publisher all read it through
// DcSettings so per-run addon overrides apply. The HP/mana recovery thresholds
// live in DungeonClearUtil::RestMin{Hp,Mp}Pct().

// --- Shared geometry bands -------------------------------------------------
// These three were file-local to DungeonClearUtil.cpp but are now shared across
// the split util units (DcEngageGeometry door/level tests, DcTargeting corridor
// scans, DcPullPlanner classification), so they live here as the single home
// (Bucket A of the constant-centralization plan).

// 2D half-width band around the walked route within which a closed door counts
// as sitting "on the corridor". Matches DOOR_CORRIDOR_WIDTH in
// DungeonClearBlockingDoorValue — a door's GO origin (its hinge/jamb) can be
// several yards off the line the bot actually walks through, so a tighter band
// misses wide gates.
constexpr float DC_DOOR_BAND = 8.0f;

// Vertical tolerance for matching a door to a leg of the route (or to a target).
// A door's GO origin Z sits at its OWN floor; the route point we test it against
// is on the walking surface. Beyond this gap the door is on a different level — a
// stacked ship deck, a ramp above/below — and must not count as blocking. The
// door tests are otherwise 2D, so without this a door directly over or under a
// point that is merely near in plan-view falsely blocks the corridor (parks the
// tank short, vetoes near-side packs). Wide enough to absorb a doorway
// threshold/lip and minor navmesh-vs-GO Z drift, tight enough to keep adjacent
// floors apart.
constexpr float DC_DOOR_Z_BAND = 6.0f;

// Below this vertical offset a candidate is treated as on the bot's own level:
// slopes, stairs and ramps stay under it within a corridor lookahead. WotLK
// inter-floor gaps are larger, so a genuine other-level mob always exceeds it.
// Shared by IsLevelReachable (trash), IsAtBossEngage (boss-arrival floor guard),
// and the dynamic-pull classification.
constexpr float DC_Z_LEVEL_TOLERANCE = 5.0f;

// Vertical half-band for matching a trash candidate to a leg of the corridor in
// the blocking-trash scans. The corridor band test is otherwise 2D, so a mob at
// the bottom of a ledge — directly under (or over) an early leg of the route in
// plan view — would match that leg and get picked NOW, even though along the
// actual route it is far beyond the lookahead (the tank reaches it only after
// winding down the ramp). With this gate the mob can only match the legs on its
// OWN floor, which the along-path lookahead then correctly defers until the
// tank actually descends. Looser than DC_Z_LEVEL_TOLERANCE to absorb
// navmesh-vs-terrain Z drift along the polyline and mobs perched on bumps.
constexpr float DC_CORRIDOR_Z_BAND = 8.0f;

// Detour gates for IsLevelReachable's cross-level path probe. A candidate on
// another vertical level only counts as reachable when the NAVIGATIONAL path to
// it is commensurate with its straight-line distance: a mob 15yd below the
// ledge whose real approach is a 70yd ramp detour merely LOOKS close and must
// not be proactively engaged/pulled (it is handled when the route brings the
// tank to its floor). The slack term keeps legitimate short around-the-corner
// detours alive at close range, where a pure ratio test is too strict.
constexpr float DC_TRASH_DETOUR_RATIO = 2.0f;
constexpr float DC_TRASH_DETOUR_SLACK = 20.0f;

// Smart Rest failsafes (DcSmartRest::UpdateLatch). A latched rest normally
// releases when every bot reaches full hp/mana — but a member that CANNOT get
// there (an AFK human who never drinks, a bot with no food when the food cheat
// is off) must not stall the run forever, so a latch is force-released after
// DC_SMART_REST_MAX_MS. The rearm cooldown then blocks an immediate re-latch on
// that same member, or the party would flap latch/timeout in a tight cycle.
// Worst case: 3-minute rest / 30-second push cycles — strictly better than the
// legacy gate, which stalls indefinitely on the same member.
constexpr uint32 DC_SMART_REST_MAX_MS   = 180000;
constexpr uint32 DC_SMART_REST_REARM_MS = 30000;

// Position-based stuck detection, shared by the Advance drive and the
// door-blocked walk-in (both glide the same escort spline). If the bot is
// supposed to be moving but its world position barely shifts for
// DC_STUCK_TICK_LIMIT consecutive ticks, treat it as wedged and recover
// (halt the spline + re-anchor the cursor) rather than blindly relaunching.
//
// DC_STUCK_DISPLACEMENT is PER TICK and must stay well below the distance a
// HEALTHY escort glide covers in one tick, or normal movement reads as stuck.
// With run speed ~7 yd/s and the ~0.2s tick cadence a gliding bot moves
// ~1.4-1.5 yd/tick — so an old 1.5yd value flagged every healthy tick. A
// genuinely wedged bot moves ~0 yd/tick, so 0.5yd cleanly separates the two:
// 3x above the wedge floor yet ~1yd below the slowest healthy glide.
constexpr float DC_STUCK_DISPLACEMENT = 0.5f;
constexpr uint32 DC_STUCK_TICK_LIMIT = 5;

#endif  // _DUNGEON_CLEAR_TUNING_H
