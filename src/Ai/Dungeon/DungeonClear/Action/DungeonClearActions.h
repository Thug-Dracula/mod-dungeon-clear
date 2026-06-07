/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARACTIONS_H
#define _PLAYERBOT_DUNGEONCLEARACTIONS_H

#include "MovementActions.h"

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
    // Per-tick approach state computed once at the top of Execute and shared
    // across the extracted phase steps below (the boss's effective live
    // position, the engage-range gates, and the at-boss predicate).
    struct AdvanceState
    {
        DungeonBossInfo const* next;
        Creature* liveBoss;
        float bossX, bossY, bossZ;
        float engageDist, engageRange;
        bool atBoss;
    };

    // A phase step either handles the tick (Execute returns the carried bool)
    // or falls through to the next phase. Keeps Execute a short, readable ladder
    // instead of one 750-line method. The counter-coupled tail (stuck recovery,
    // direct pursuit, long-path drive) is still inline below the ladder.
    enum class Step { Continue, ReturnTrue, ReturnFalse };

    Step TryEngageHold(AdvanceState const& st);
    Step TryLootYield(AdvanceState const& st);
    Step TryBetweenPullsRest(AdvanceState const& st);
    Step TryBossNotPresentStall(AdvanceState const& st);
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
// the bot passive (DungeonClearUtil::ApplyFollowerPassive), cancels any stale
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

// Shared body for the two follower-only "join the out-of-LOS camp fight" actions.
// Resolves the nearest live unit attacking the leader tank — LINE-OF-SIGHT BLIND
// on purpose — sets it as the bot's current target, forces the bot into combat
// with it, and moves the bot onto it. This is the fix for a camp parked near a
// corner: the drag-back can leave the pack out of the camp's line of sight, where
// the stock LOS-gated target picker never acquires it and the released party
// stands idle. Moving the follower in regains sight, at which point its trigger
// goes inert (a valid attacker is now visible) and stock combat owns the fight.
// Registered under two names on the two engines (see the subclasses below).
class DungeonClearAssistCampActionBase : public MovementAction
{
public:
    DungeonClearAssistCampActionBase(PlayerbotAI* botAI, std::string const& name)
        : MovementAction(botAI, name)
    {
    }
    bool Execute(Event event) override;
};

// Non-combat engine: a follower that never took a hit, sitting idle at camp.
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

#endif
