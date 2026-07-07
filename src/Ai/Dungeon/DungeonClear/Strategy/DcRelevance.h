/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRELEVANCE_H
#define _PLAYERBOT_DCRELEVANCE_H

// The dungeon-clear trigger relevance ladder, as named constants shared by BOTH
// strategies (DungeonClearStrategy = non-combat, DungeonClearCombatStrategy =
// combat) instead of float literals scattered across the InitTriggers bodies
// with the invariants living only in prose. t/TestRelevanceLadder.cpp pins the
// ordering and documents every same-value tie with the partition (role / engine /
// map / anchor-kind) that legitimizes it, so a future edit that reorders a rung —
// or lands a new feature on an occupied one — trips a red test instead of
// silently depending on trigger registration order.
//
// TIES THAT ARE REAL (measured, arch-review F3) and how they are handled:
//   - Chat == PartyDied (100): distinct trigger conditions (keyword vs death),
//     both terminal — kept equal, asserted.
//   - AtBoss == AtObjective (30): mutually exclusive by the anchor-kind check in
//     each trigger — kept equal, asserted.
//   - BlockingTrash == FollowTank (25): leader-only vs follower-only — kept
//     equal, asserted.
//   - PullManeuver == StayAtCamp (60, combat): leader-only vs follower-only —
//     kept equal, asserted.
// TIES THAT WERE NOT PARTITIONED (both armable on the same leader) — BROKEN here
// so ordering is deterministic:
//   - HakkarFlame was 35, tying Pull (35). Now 35.5 (still below the 36
//     suppressor, above the 34 loot-blood): on the Sunken Temple carrier a douse
//     outranks starting a fresh pull.
//   - NeedsRest (drink/eat) was 26, tying RoomTrash (26). Now 26.5: a leader
//     below its rest target tops up before committing to a room pre-clear,
//     matching the rest trigger's "top up before pulling" intent.
namespace DcRel
{
    // ===== shared top band =====
    inline constexpr float Chat            = 100.0f; // chat keyword commands (dc on/off/…)
    inline constexpr float PartyDied       = 100.0f; // death bailout (non-combat only)
    inline constexpr float LootRollPending = 95.0f;  // resolve an open loot-roll window at once
    inline constexpr float DoorReopened    = 90.0f;  // auto-resume when a player opens the door
    inline constexpr float AllCleared      = 50.0f;  // congratulate + disable
    inline constexpr float HealReposition  = 41.0f;  // healer-only; both engines (see note below)

    // ===== non-combat leader driving ladder =====
    inline constexpr float HakkarSuppressor = 36.0f; // ST Hakkar: silence a resetting suppressor
    inline constexpr float HakkarFlame      = 35.5f; // ST Hakkar: douse (tie-broken above Pull)
    inline constexpr float Pull             = 35.0f; // advanced/dynamic pull-to-camp maneuver
    inline constexpr float HakkarLootBlood  = 34.0f; // ST Hakkar: grab blood (below flame)
    inline constexpr float EventDue         = 31.0f; // off-path conditional event gate due
    inline constexpr float AtBoss           = 30.0f; // engage the next boss
    inline constexpr float AtObjective      = 30.0f; // arrive at a travel objective (anchor-kind peer)
    inline constexpr float AssistCamp       = 29.0f; // follower: pile into the leader's fight
    inline constexpr float HoldAtCamp       = 28.0f; // follower: hold passive at camp mid-pull
    inline constexpr float NeedsRest        = 26.5f; // rest to target HP/mana (tie-broken above RoomTrash)
    inline constexpr float RoomTrash        = 26.0f; // room-aggro boss pre-clear
    inline constexpr float BlockingTrash    = 25.0f; // leader: engage a pack blocking the path
    inline constexpr float FollowTank       = 25.0f; // follower: redirect follow to the tank
    inline constexpr float LeaderAssist     = 24.0f; // leader: help a fight it never saw
    inline constexpr float DoorBlocked      = 22.0f; // stall at a shut door
    inline constexpr float Stalled          = 20.0f; // stalled-no-path fallback
    inline constexpr float RoomPreclearHold = 16.0f; // hold the room-aggro standoff
    inline constexpr float Advance          = 15.0f; // default: walk toward the next boss
    inline constexpr float FilterLoot       = 9.0f;  // enforce loot policy while paused

    // ===== combat engine =====
    inline constexpr float HakkarSuppressorCombat = 64.0f; // ST Hakkar combat side
    inline constexpr float HakkarFlameCombat      = 63.0f;
    inline constexpr float HakkarLootBloodCombat  = 62.0f;
    inline constexpr float PullManeuver           = 60.0f; // leader: drag the pack back to camp
    inline constexpr float StayAtCamp             = 60.0f; // follower: pin at camp (role peer of PullManeuver)
    inline constexpr float AssistCampCombat       = 35.0f; // follower: onto the leader's pack
    inline constexpr float RegroupCombat          = 33.0f; // follower: close back onto the leash
    // HealReposition (41) also registers in the combat engine, ABOVE AssistCampCombat
    // (35) / RegroupCombat (33) and the stock reach-heal (40), BELOW the camp owners
    // (60). Healer-only, so it never contends with the leader-only camp owners.
}

#endif
