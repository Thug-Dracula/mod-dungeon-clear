/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_LEADER_SIGNAL_H
#define _DC_LEADER_SIGNAL_H

#include "Define.h"
#include "Position.h"

class Player;

class DcLeaderSignal
{
public:
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

    // --- Advanced pulls -----------------------------------------------------
    // True for the "holding" pull phases (Forming/Advancing/Returning) during
    // which the party stays passive and camped; false for Idle/Engage.
    static bool IsPullPhaseHolding(uint32 phase);

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

    // The GENERAL "tank is fighting -> the party assists" gate: true when `bot`'s
    // elected leader tank is in combat on an active (enabled, unpaused) run and
    // the party is expected to pile in RIGHT NOW. Includes the advanced-pull camp
    // fight (IsLeaderCampFightActive), but ALSO every fight the camp machinery
    // does not own: pull mode off, a Leeroy verdict in dynamic mode, boss
    // walk-ins, and unplanned aggro outside a camp hold. This is what covers a
    // tank that aggros around a corner or beyond a follower's natural engage
    // range — group combat never propagates that far, and DC's multiplier mutes
    // the stock proactive pickers, so without this push the party stands idle
    // while the tank solos. Defers (false) while an advanced-pull camp hold is
    // in effect outside the camp fight: the holding phases pin the party passive
    // and an Idle-phase aggro is dragged back to camp first. False for the
    // leader itself, off/paused runs, or a leader out of combat. Drives
    // DungeonClearAssistCamp{,Combat}Trigger.
    static bool IsLeaderFightAssistWanted(Player* bot);

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

    // --- Room-aggro clear ----------------------------------------------------
    // True when `bot`'s elected leader tank is mid room-aggro clear (a flagged
    // RoomAggroRegistry boss with room trash still to clear, the tank at the boss
    // room) — the window in which the tank skirts the boss's aggro sphere on its
    // own approach (DungeonClearEngageActionBase::RoomAggroSkirtPoint). Writes the
    // LIVE boss centre to `centerOut` and the avoid-sphere radius (the boss's real
    // aggro range for THIS follower + both reaches + AggroRangeMargin +
    // RoomAggroPathPadding — the same sizing the tank's skirt uses) to `radiusOut`.
    // Followers read this so their close-follow to the tank can detour AROUND the
    // sphere instead of cutting a straight line through it: the tank dodges the
    // boss correctly, but a follower bee-lining to a tank parked on the far side of
    // the sphere would otherwise run the party into aggro and wake the room. Reads
    // the leader's context cross-bot (same pattern as IsLeaderDynamicScouting);
    // pass any group member. Returns false (outputs untouched) for the leader
    // itself, off/paused runs, when no room clear is active, or when the boss isn't
    // loaded.
    static bool GetLeaderRoomAggroSphere(Player* bot, Position& centerOut,
                                         float& radiusOut);

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

};

#endif  // _DC_LEADER_SIGNAL_H
