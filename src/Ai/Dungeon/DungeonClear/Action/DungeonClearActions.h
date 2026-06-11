/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARACTIONS_H
#define _PLAYERBOT_DUNGEONCLEARACTIONS_H

#include "MovementActions.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"

class PlayerbotAI;
class Unit;
class Creature;
struct DungeonBossInfo;

// Shared base for engage actions: walks into attack range and forces combat
// via bot->Attack directly. We deliberately bypass the pull pipeline — its
// reach/cast handshake has a dead zone where the bot is too far for the
// pull-range check but too close for ReachCombatTo to move, leaving the tank
// standing in sight of a mob just outside aggro range.
class DungeonClearEngageActionBase : public MovementAction
{
public:
    DungeonClearEngageActionBase(PlayerbotAI* botAI, std::string const name) : MovementAction(botAI, name) {}

protected:
    // Returns true if the bot took action (moved or attacked). Caller passes
    // the target it picked; this routine handles the movement-then-attack
    // sequence regardless of whether the target is a boss, blocking trash,
    // or a stalled-fallback obstacle.
    bool EngageDirect(Unit* target);
};

class DungeonClearAdvanceAction : public MovementAction
{
public:
    DungeonClearAdvanceAction(PlayerbotAI* botAI) : MovementAction(botAI, "dungeon clear advance") {}
    bool Execute(Event event) override;

private:
    // Per-tick approach state computed at the top of Execute and threaded
    // through the extracted phase steps below. The boss snapshot (effective live
    // position, the engage-range gates, the at-boss predicate) is filled before
    // the ladder; the owned approach FSM (appr), the resolved long-path + its
    // follower cursor, and the current hop are filled as Execute walks the ladder
    // and the later phases consume them. Carried as pointers/value (not refetched
    // per phase) because the single NextHop call has follower side effects, so
    // the hop cannot be recomputed phase-by-phase.
    struct AdvanceState
    {
        DungeonBossInfo const* next = nullptr;
        Creature* liveBoss = nullptr;
        float bossX = 0.0f, bossY = 0.0f, bossZ = 0.0f;
        float engageDist = 0.0f, engageRange = 0.0f;
        bool atBoss = false;

        DcApproachState* appr = nullptr;                    // owned approach FSM state
        ChunkedPathfinder::Result const* path = nullptr;    // resolved after EnsureLongPath
        DungeonFollowerState* follower = nullptr;           // long-path follower cursor
        DungeonPathFollower::Hop hop;                       // current hop (single NextHop)
    };

    // A phase step either handles the tick (Execute returns the carried bool)
    // or falls through to the next phase. Keeps Execute a short, readable ladder
    // of Try* rungs instead of one 750-line method.
    enum class Step { Continue, ReturnTrue, ReturnFalse };

    // Pre-route phases (boss snapshot only).
    Step TryEngageHold(AdvanceState const& st);
    Step TryLootYield(AdvanceState const& st);
    Step TryBetweenPullsRest(AdvanceState const& st);
    Step TryBossNotPresentStall(AdvanceState const& st);

    // Counter-coupled tail, now extracted in declared order. These read/write
    // the shared approach state, long-path, follower, and hop carried in st.
    Step TryPosStuckRecovery(AdvanceState& st);
    Step TryDirectPursuit(AdvanceState& st);
    Step TryLongPathUnreachable(AdvanceState& st);
    Step TryOffPathResnap(AdvanceState& st);
    Step TryReanchorStaleCursor(AdvanceState& st);   // never terminates; only re-anchors the cursor
    Step TryHopDoneEscalation(AdvanceState& st);
    Step TryJumpLeg(AdvanceState& st);
    Step TryRideLiveGlide(AdvanceState& st);
    Step TryOffLineRejoin(AdvanceState& st);
    Step TrySplineWindowIssue(AdvanceState& st);
    Step TryMoveToFallback(AdvanceState& st);        // terminal: always handles the tick
};

class DungeonClearEngageTrashAction : public DungeonClearEngageActionBase
{
public:
    DungeonClearEngageTrashAction(PlayerbotAI* botAI) : DungeonClearEngageActionBase(botAI, "dungeon clear engage trash") {}
    bool Execute(Event event) override;
};

class DungeonClearEngageBossAction : public DungeonClearEngageActionBase
{
public:
    DungeonClearEngageBossAction(PlayerbotAI* botAI) : DungeonClearEngageActionBase(botAI, "dungeon clear engage boss") {}
    bool Execute(Event event) override;
};

// Clears a room-wide-aggro boss's room (RoomAggroRegistry) before the boss is
// pulled, for the Leeroy case (pull mode current false: Off, or Dynamic chose
// Leeroy). Walks the tank to the NEAREST remaining room-trash unit and tanks it
// in place via EngageDirect — nearest-first so the tank works the room from its
// edge inward and the boss's own aggro sphere (excluded from the remaining set)
// is approached last, never waking the boss. When pull-to-camp is in effect the
// pull pipeline owns the room clear instead (this trigger stands down). Sits at
// relevance 26 (between engage-trash 25 and engage-boss 30).
class DungeonClearRoomClearAction : public DungeonClearEngageActionBase
{
public:
    DungeonClearRoomClearAction(PlayerbotAI* botAI)
        : DungeonClearEngageActionBase(botAI, "dungeon clear room clear")
    {
    }
    bool Execute(Event event) override;
};

// Fallback when the tank can't path to the next boss. Picks the closest
// reachable hostile anywhere on the map and pulls it; clearing obstacles
// usually unblocks the path on the next advance tick.
class DungeonClearClearStalledAction : public DungeonClearEngageActionBase
{
public:
    DungeonClearClearStalledAction(PlayerbotAI* botAI) : DungeonClearEngageActionBase(botAI, "dungeon clear clear stalled") {}
    bool Execute(Event event) override;
};

// Run on non-tank party bots while their tank is in DC mode. Redirects follow
// from the player master to the tank so the party stays with whoever is
// leading the clear.
class DungeonClearFollowTankAction : public MovementAction
{
public:
    DungeonClearFollowTankAction(PlayerbotAI* botAI) : MovementAction(botAI, "dungeon clear follow tank") {}
    bool Execute(Event event) override;
};

// Drives a travel OBJECTIVE (DungeonAnchorKind::Objective from
// BossRosterRegistry) once the tank has reached it (DungeonClearAtObjectiveTrigger
// fired). Runs the objective's optional on-arrival hook (ObjectiveHookRegistry):
// on Done / no hook it latches the anchor into "dungeon clear cleared anchors"
// so NextDungeonBossValue advances to the next target; on Running it holds the
// tank at the anchor; on Blocked it stalls the run for the human. Never engages
// combat — objectives are not creatures.
class DcObjectiveArriveAction : public MovementAction
{
public:
    DcObjectiveArriveAction(PlayerbotAI* botAI) : MovementAction(botAI, "dungeon clear objective arrive") {}
    bool Execute(Event event) override;
};

class DungeonClearDisableOnDeathAction : public Action
{
public:
    DungeonClearDisableOnDeathAction(PlayerbotAI* botAI) : Action(botAI, "dungeon clear disable on death") {}
    bool Execute(Event event) override;
};

class DungeonClearDisableOnClearedAction : public Action
{
public:
    DungeonClearDisableOnClearedAction(PlayerbotAI* botAI) : Action(botAI, "dungeon clear disable on cleared") {}
    bool Execute(Event event) override;
};

// Walks the tank up to the blocking door, then stalls with an explicit
// "door is closed" message in party chat. The door is detected up to 80yd
// ahead, so without the walk-in the tank would park wherever it was when the
// door entered look-ahead — often far short of the door. Bot stays enabled so
// the player can open the door and the next tick resumes; only the
// position-stuck recovery or `dc off` cancels.
class DungeonClearDoorBlockedAction : public MovementAction
{
public:
    DungeonClearDoorBlockedAction(PlayerbotAI* botAI) : MovementAction(botAI, "dungeon clear door blocked") {}
    bool Execute(Event event) override;
};

// Keeps the DC loot policy (DungeonClear.LootMinQuality / IgnoreChests) enforced
// while the run is PAUSED. The driving ladder is inert when paused, so the
// loot-floor filter that normally runs inline in advance/follow-tank never gets
// a tick and the bot reverts to the stock playerbots loot pipeline. This action
// runs that same filter above the loot pipeline's relevance, then returns false
// so the stock loot actions still collect whatever survives the filter.
class DungeonClearFilterLootAction : public Action
{
public:
    DungeonClearFilterLootAction(PlayerbotAI* botAI) : Action(botAI, "dungeon clear filter loot") {}
    bool Execute(Event event) override;
};

// --- Advanced pulls -------------------------------------------------------
// Leader-only, non-combat. The out-of-combat half of the pull-to-camp maneuver:
//   Idle      -> stamp camp at the tank's spot, signal Forming.
//   Forming   -> hold a beat so followers go passive, then -> Advancing.
//   Advancing -> run in to the trash pack to grab aggro. The moment combat
//                starts, control passes to DungeonClearPullManeuverAction on the
//                combat engine. Aborts (-> normal walk-in engage) if the run-in
//                wedges or overshoots without aggroing.
//   Engage    -> out-of-combat cleanup: reset to Idle so the next pull is fresh.
class DungeonClearPullAction : public DungeonClearEngageActionBase
{
public:
    DungeonClearPullAction(PlayerbotAI* botAI) : DungeonClearEngageActionBase(botAI, "dungeon clear pull") {}
    bool Execute(Event event) override;
};

// Leader-only, COMBAT engine. The in-combat half of the maneuver: once aggro is
// confirmed it runs the tank back to the camp (suppressing stock chase/attack by
// owning the tick), then hands the fight to stock combat at camp (phase Engage),
// which is also when ReapStrandedPassives releases the party. Gives up to fight
// in place if the return leg wedges.
class DungeonClearPullManeuverAction : public MovementAction
{
public:
    DungeonClearPullManeuverAction(PlayerbotAI* botAI) : MovementAction(botAI, "dungeon clear pull maneuver") {}
    bool Execute(Event event) override;
};

// Shared body for the two follower-only "hold the party at camp" actions. Puts
// the bot passive (DcFollowerLifecycle::ApplyFollowerPassive), cancels any stale
// follow generator, walks it to the leader's camp, and OWNS the tick (always
// returns true while the leader is in a holding pull phase) so neither
// follow-tank nor stock follow can drag the follower off camp. The two concrete
// subclasses below register this same body under different names on different
// engines.
class DungeonClearCampHoldActionBase : public MovementAction
{
public:
    DungeonClearCampHoldActionBase(PlayerbotAI* botAI, std::string const& name)
        : MovementAction(botAI, name)
    {
    }
    bool Execute(Event event) override;
};

// Non-combat engine: holds the party at camp while the leader pulls and the
// follower is OUT of combat (DungeonClearHoldAtCampTrigger).
class DungeonClearHoldAtCampAction : public DungeonClearCampHoldActionBase
{
public:
    DungeonClearHoldAtCampAction(PlayerbotAI* botAI)
        : DungeonClearCampHoldActionBase(botAI, "dungeon clear hold at camp")
    {
    }
};

// Combat engine: the same hold, for when the follower is IN combat. A held
// follower enters combat the instant the tank does (group combat), which
// switches it to the combat engine where the non-combat hold can't run AND
// PassiveMultiplier explicitly green-lights stock "follow" — so without this the
// party trails the tank the moment a pull aggros. The action NAME deliberately
// contains "stay" so PassiveMultiplier's substring whitelist lets it run while
// the follower is +passive; registered above the stock combat movers so it owns
// the tick and pins the follower at camp until release. See
// DungeonClearHoldAtCampCombatTrigger.
class DungeonClearStayAtCampAction : public DungeonClearCampHoldActionBase
{
public:
    DungeonClearStayAtCampAction(PlayerbotAI* botAI)
        : DungeonClearCampHoldActionBase(botAI, "dungeon clear stay at camp")
    {
    }
};

// Shared body for the two follower-only "join the leader's fight" actions.
// Resolves the nearest live unit attacking the leader tank — LINE-OF-SIGHT BLIND
// on purpose — sets it as the bot's current target, forces the bot into combat
// with it, and moves the bot onto it. This is the fix for a fight the follower
// can't see or reach: a camp parked near a corner, or a Leeroy/dynamic/boss
// pull the tank took around a corner or beyond the follower's natural engage
// range — anywhere the stock LOS-gated target picker never acquires the pack
// and the party stands idle while the tank solos. Moving the follower in
// regains sight, at which point its trigger goes inert (a valid attacker is
// now visible) and stock combat owns the fight. Gated by
// DcLeaderSignal::IsLeaderFightAssistWanted; registered under two names on the
// two engines (see the subclasses below).
class DungeonClearAssistCampActionBase : public MovementAction
{
public:
    DungeonClearAssistCampActionBase(PlayerbotAI* botAI, std::string const& name)
        : MovementAction(botAI, name)
    {
    }
    bool Execute(Event event) override;
};

// Non-combat engine: a follower that never took a hit, sitting idle while the
// leader tank fights.
class DungeonClearAssistCampAction : public DungeonClearAssistCampActionBase
{
public:
    DungeonClearAssistCampAction(PlayerbotAI* botAI)
        : DungeonClearAssistCampActionBase(botAI, "dungeon clear assist camp")
    {
    }
};

// Combat engine: a follower dragged into combat but with the pack out of sight,
// idling in the combat engine with an empty LOS attacker list.
class DungeonClearAssistCampCombatAction : public DungeonClearAssistCampActionBase
{
public:
    DungeonClearAssistCampCombatAction(PlayerbotAI* botAI)
        : DungeonClearAssistCampActionBase(botAI, "dungeon clear assist camp combat")
    {
    }
};

// Combat engine: a follower that has drifted too far from, or out of LOS of, the
// leader tank during a fight. Closes back on the tank (stopping a few yards short)
// so the party stays grouped and a stranded healer regains line of sight to its
// heal target. Driven by DungeonClearRegroupCombatTrigger.
class DungeonClearRegroupCombatAction : public MovementAction
{
public:
    DungeonClearRegroupCombatAction(PlayerbotAI* botAI)
        : MovementAction(botAI, "dungeon clear regroup combat")
    {
    }
    bool Execute(Event event) override;
};

#endif
