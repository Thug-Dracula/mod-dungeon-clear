/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearStrategy.h"

#include "Ai/Dungeon/DungeonClear/Multiplier/DungeonClearMultiplier.h"
#include "Playerbots.h"

void DungeonClearStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Highest priority: bail out on death.
    triggers.push_back(new TriggerNode(
        "dungeon clear party died",
        { NextAction("dungeon clear disable on death", 100.0f) }));

    // All bosses cleared — congratulate and disable.
    triggers.push_back(new TriggerNode(
        "dungeon clear all cleared",
        { NextAction("dungeon clear disable on cleared", 50.0f) }));

    // Advanced pull (LOS pull-to-camp). Sits ABOVE the engage triggers so, when
    // pull mode is on, the tank runs the pull-to-camp maneuver instead of the
    // normal walk-in — but it is trash-only (the pull-target scan vetoes dungeon
    // bosses outright, and the trigger additionally stands down inside boss
    // engage range), so the at-boss engage below still owns boss pulls. Inert
    // when pull mode is off. See DungeonClearPullTrigger / DungeonClearPullAction.
    triggers.push_back(new TriggerNode(
        "dungeon clear pull",
        { NextAction("dungeon clear pull", 35.0f) }));

    // Off-path CONDITIONAL event due (DungeonEventRegistry): a pre-boss gate the
    // party must perform — pull a lever, talk to a prisoner to open the gate, etc.
    // Relevance 31, just above the at-boss pull (30), so a due gate preempts the
    // boss engage AND the door-blocked stall (22). Inert unless a conditional
    // event's condition is currently true and un-latched. See
    // DungeonClearEventDueTrigger / DcRunEventAction.
    triggers.push_back(new TriggerNode(
        "dungeon clear event due",
        { NextAction("dungeon clear run event", 31.0f) }));

    // Within engage range of next boss.
    triggers.push_back(new TriggerNode(
        "dungeon clear at boss",
        { NextAction("dungeon clear engage boss", 30.0f) }));

    // Sunken Temple (map 109) Avatar of Hakkar encounter handlers. These live in
    // BOTH strategies: here for the brief out-of-combat gaps between waves, and
    // (the important copy) in DungeonClearCombatStrategy so they actually run mid-
    // fight — the whole encounter is a wave fight, so a bot is almost always in
    // combat, and the previous non-combat-only registration was why the flames
    // never got doused (the win path) and the fight just timed out. Inert
    // everywhere but the live Sanctum (the triggers gate on it).
    //
    // Priority: suppressor (36) > flame (35) > loot blood (34).
    //  - Suppressor first: a Nightmare Suppressor left channelling RESETS the
    //    event; merely tagging it (its drain is an OOC channel) silences it.
    //  - Flame ABOVE loot blood: once a bot HOLDS blood, dousing makes progress
    //    toward the 4/4 that spawns the Avatar — it must not keep grabbing more
    //    blood (the old loot>flame order starved the douse, the reported "trouble
    //    getting priority to extinguish the flames"). The flame trigger only fires
    //    when the bot already carries blood, so a bot WITHOUT blood still loots.
    triggers.push_back(new TriggerNode(
        "dungeon clear hakkar suppressor",
        { NextAction("dungeon clear hakkar suppressor", 36.0f) }));
    triggers.push_back(new TriggerNode(
        "dungeon clear hakkar flame",
        { NextAction("dungeon clear hakkar flame", 35.0f) }));
    triggers.push_back(new TriggerNode(
        "dungeon clear hakkar loot blood",
        { NextAction("dungeon clear hakkar loot blood", 34.0f) }));

    // Arrived at a travel OBJECTIVE (BossRosterRegistry, non-combat anchor).
    // Peer of at-boss (30) — mutually exclusive via the anchor-kind check in
    // each trigger — so the objective is completed and the clear advances
    // instead of trying to engage a non-creature target.
    triggers.push_back(new TriggerNode(
        "dungeon clear at objective",
        { NextAction("dungeon clear objective arrive", 30.0f) }));

    // Blocking trash on the path to the next boss.
    triggers.push_back(new TriggerNode(
        "dungeon clear blocking trash",
        { NextAction("dungeon clear engage trash", 25.0f) }));

    // Room-wide-aggro boss pre-clear (RoomAggroRegistry): at a flagged boss, clear
    // the room before the boss is pulled. Relevance 26 — between engage-trash (25)
    // and engage-boss (30) — so it preempts the (stood-down) corridor scan and the
    // boss pull is held by the at-boss gate until the room is clear. This node is
    // the Off / Dynamic-chose-Leeroy path (walk in and tank in place); when
    // pull-to-camp is in effect the pull pipeline (35) owns the room clear instead.
    triggers.push_back(new TriggerNode(
        "dungeon clear room trash",
        { NextAction("dungeon clear room clear", 26.0f) }));

    // Stalled fallback: only fires when Advance/EngageBoss has set a stall
    // reason because no path to the next boss exists. Sits above the default
    // advance (15) so the fallback kill wins, and below at-boss (30) and
    // blocking-trash (25) so a viable boss/trash pull still preempts.
    triggers.push_back(new TriggerNode(
        "dungeon clear stalled",
        { NextAction("dungeon clear clear stalled", 20.0f) }));

    // Door blocking the corridor: stall with a specific message. Sits
    // above advance (15) but below the engage triggers so a hostile in the
    // doorway still gets pulled first; otherwise the bot stops and waits
    // for the door to be opened.
    triggers.push_back(new TriggerNode(
        "dungeon clear door blocked",
        { NextAction("dungeon clear door blocked", 22.0f) }));

    // LEADER-only: a groupmate is fighting a pack the tank never saw (a follower
    // aggroed around a sharp corner, or the tank called the pull done and walked
    // off toward the next objective) — so rather than freezing on the Advance rest
    // gate ("party not ready / resting") while the DPS fight without it, the tank
    // goes back and takes threat. Relevance 24: above advance (15), the stalled
    // fallback (20) and door-blocked (22) — all of which would otherwise leave the
    // tank stranded while the party fights — but BELOW the tank's own engage scans
    // (engage trash 25, room trash 26, engage boss 30), so a deliberate visible
    // pull always wins and this only fills the out-of-sight gap. Inert for
    // followers (their IsLeaderFightAssistWanted path owns them) and the instant
    // the tank sees a target of its own. See DcLeaderSignal::IsLeaderShouldAssistFight.
    triggers.push_back(new TriggerNode(
        "dungeon clear leader assist",
        { NextAction("dungeon clear leader assist", 24.0f) }));

    // Auto-resume once a player opens the door we auto-paused at. Fires only
    // while paused for that specific door (see DungeonClearDoorReopenedTrigger),
    // when the rest of the driving ladder is inert, so its relevance only has to
    // clear the stock wander/idle actions — keep it high so nothing preempts the
    // resume.
    triggers.push_back(new TriggerNode(
        "dungeon clear door reopened",
        { NextAction("dungeon clear door reopened", 90.0f) }));

    // Room pre-clear OWNER (fix #2). Active for the whole pre-clear window (flagged
    // room-aggro boss, trash still up, tank at the standoff). Relevance 16 — just
    // ABOVE the default Advance (15) and BELOW every real driver (engage/door/stall/
    // assist 20-35) — so whenever no higher driver claims the tick it HOLDS the tank
    // at the standoff instead of letting the room-aggro-blind Advance creep at the
    // boss centre. This is the structural close for the recurring "boss woken
    // mid-clear" failures: the standoff is owned every gap, not just when Advance's
    // own conditional engage-hold rung happens to fire.
    triggers.push_back(new TriggerNode(
        "dungeon clear room preclear hold",
        { NextAction("dungeon clear room preclear hold", 16.0f) }));

    // Default: walk toward the next boss. Lowest of the bunch but above
    // grind (4) / new rpg (11). Wander strategies are also suppressed by
    // DungeonClearMultiplier while enabled.
    triggers.push_back(new TriggerNode(
        "dungeon clear idle",
        { NextAction("dungeon clear advance", 15.0f) }));

    // Non-tank bots in the tank's party redirect their follow target to the
    // tank while DC is on. Relevance above the default follow (1.0) so it
    // preempts the usual master-follow behavior.
    triggers.push_back(new TriggerNode(
        "dungeon clear follow tank",
        { NextAction("dungeon clear follow tank", 25.0f) }));

    // While the leader is mid-pull, non-leader followers hold passive at the camp
    // instead of trailing the tank into the pull. Relevance above follow-tank (25)
    // so it preempts the trail for the duration of the maneuver; inert otherwise.
    triggers.push_back(new TriggerNode(
        "dungeon clear hold at camp",
        { NextAction("dungeon clear hold at camp", 28.0f) }));

    // Leader-fight assist: while the leader tank is in combat, every follower
    // still OUT of combat is driven into the fight — the advanced-pull camp
    // fight, but also any Leeroy/dynamic/boss pull the tank took around a corner
    // or beyond the follower's natural engage range, where group combat never
    // propagates and the stock target picker (LOS-filtered, and multiplier-
    // suppressed anyway) would never acquire it. Relevance above hold-at-camp
    // (28) so it preempts the camp yield, and above the rest triggers (26) so
    // "tank is fighting" outranks topping up. Defers to the camp hold during
    // the passive pull phases. See DungeonClearAssistCampTrigger /
    // DcLeaderSignal::IsLeaderFightAssistWanted.
    triggers.push_back(new TriggerNode(
        "dungeon clear assist camp",
        { NextAction("dungeon clear assist camp", 29.0f) }));

    // Rest-target override: top up to the run's chosen HP/mana before pulling.
    // Relevance above advance (15) and follow-tank (25) so a bot below target
    // sits and rests instead of walking; safely below the engage triggers,
    // which can't fire anyway while the party is still recovering (the rest gate
    // uses the same target). Only active when the run sets RestHealthPct /
    // RestManaPct; otherwise the triggers are inert and stock rest is unchanged.
    triggers.push_back(new TriggerNode(
        "dungeon clear needs drink",
        { NextAction("drink", 26.0f) }));
    triggers.push_back(new TriggerNode(
        "dungeon clear needs eat",
        { NextAction("food", 26.0f) }));

    // Keep the DC loot policy (quality floor / IgnoreChests) enforced for the
    // WHOLE party while the run is PAUSED — leader and followers alike. The
    // driving ladder above is inert when paused and followers stop trailing the
    // leader, so without this every member reverts to the stock playerbots loot
    // pipeline and loots everything (and follower junk-looting stalls the tank
    // via IsAnyPartyMemberLooting). Relevance 9 sits just above the stock loot
    // actions (open loot is 8) so the filter prunes before they pick up; the
    // action returns false so the surviving loot is still collected this tick.
    // Inert unless paused — when active the same filter runs inline in
    // advance/follow-tank. See DungeonClearFilterLootTrigger.
    triggers.push_back(new TriggerNode(
        "dungeon clear filter loot",
        { NextAction("dungeon clear filter loot", 9.0f) }));

    // BetterLootRolling improvement #3: roll the moment a loot-roll window
    // opens. Stock only reaches "loot roll" off the "very often" RandomTrigger
    // (1-in-3 at most every RepeatDelay), leaving bots staring at an open roll
    // for many seconds. This node drives the same action — the
    // BetterLootRollAction override — every tick a vote is pending. The action
    // is instant and resolves one roll per execute, so the trigger self-clears;
    // relevance above the whole driving ladder (door reopened 90) so the vote
    // never queues behind movement, below chat (100). Inert unless
    // DungeonClear.BetterLootRolling is on (see the trigger).
    triggers.push_back(new TriggerNode(
        "dungeon clear loot roll pending",
        { NextAction("loot roll", 95.0f) }));

    // Chat-keyword triggers (`dc on/off/skip/status/bosses` + long aliases).
    // Folded in here so there is a single "dungeon clear" strategy: one name to
    // apply (via config or the login hook), which is what lets self-bots —
    // built from config when `.playerbots bot self` is toggled — pick up the
    // whole feature, keyword listener included. ChatCommandTrigger latches its
    // fired flag until an engine checks it, so a `dc off` typed mid-combat still
    // fires the moment the bot next ticks the non-combat engine.
    constexpr float chatRel = 100.0f;
    triggers.push_back(new TriggerNode("dc on",             { NextAction("dc on",     chatRel) }));
    triggers.push_back(new TriggerNode("dungeon clear on",  { NextAction("dc on",     chatRel) }));
    triggers.push_back(new TriggerNode("dc off",            { NextAction("dc off",    chatRel) }));
    triggers.push_back(new TriggerNode("dungeon clear off", { NextAction("dc off",    chatRel) }));
    triggers.push_back(new TriggerNode("dc skip",           { NextAction("dc skip",   chatRel) }));
    // Single toggle: pauses when running, resumes when paused.
    triggers.push_back(new TriggerNode("dc pause",            { NextAction("dc pause", chatRel) }));
    triggers.push_back(new TriggerNode("dungeon clear pause", { NextAction("dc pause", chatRel) }));
    // Advanced-pull toggle (`dc pull [on|off]`). The keyword trigger keys differ
    // from the engine "dungeon clear pull" trigger to avoid a creator collision:
    // "dungeon clear pull keyword" listens for the chat phrase "dungeon clear pull".
    triggers.push_back(new TriggerNode("dc pull",                  { NextAction("dc pull", chatRel) }));
    triggers.push_back(new TriggerNode("dungeon clear pull keyword", { NextAction("dc pull", chatRel) }));
    triggers.push_back(new TriggerNode("dc status",         { NextAction("dc status", chatRel) }));
    triggers.push_back(new TriggerNode("dc bosses",         { NextAction("dc bosses", chatRel) }));
}

void DungeonClearStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonClearMultiplier(botAI));
}

void DungeonClearCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // The in-combat half of the advanced pull: once the tank aggros, run it back
    // to the camp before releasing the party. Relevance above the stock combat
    // movement/attack actions (MoveChase ~30, attack lines) so the maneuver owns
    // the tick and the tank doesn't fight at the pack. Inert unless the bot is the
    // leader and mid-pull (see DungeonClearPullManeuverTrigger).
    triggers.push_back(new TriggerNode(
        "dungeon clear pull maneuver",
        { NextAction("dungeon clear pull maneuver", 60.0f) }));

    // Combat-engine hold for held FOLLOWERS. A held follower enters combat the
    // instant the tank aggros (group combat) and switches to this engine, where
    // the non-combat hold-at-camp can't run and PassiveMultiplier explicitly
    // permits stock "follow" while the bot is +passive — so without this the
    // party trails the tank the moment a pull starts (the "passive isn't enough"
    // bug). The action name contains "stay" so PassiveMultiplier's substring
    // whitelist lets it through; relevance above the stock combat movers (follow
    // 1.0, move-from-group 1.0, MoveChase ~30) so it owns the tick and pins the
    // follower at camp. Inert at Engage (not a holding phase), so the released
    // party fights normally. Leader-exempt via the trigger.
    triggers.push_back(new TriggerNode(
        "dungeon clear stay at camp",
        { NextAction("dungeon clear stay at camp", 60.0f) }));

    // Leader-fight assist, combat-engine side. A follower that was dragged into
    // combat (group combat / stray hit) but has the pack around a corner has an
    // empty LOS attacker list and so idles in the combat engine — stock
    // MoveChase/attack have no target. Drive it onto the leader's pack to
    // regain sight; fires for ANY leader fight, not just the camp fight. Relevance
    // above the stock combat movers (MoveChase ~30) so it owns the tick; inert
    // the instant a valid attacker is visible, handing back to stock combat. Sits
    // below stay-at-camp / pull-maneuver (60), which are inert at Engage anyway.
    triggers.push_back(new TriggerNode(
        "dungeon clear assist camp combat",
        { NextAction("dungeon clear assist camp combat", 35.0f) }));

    // In-combat regroup for FOLLOWERS: keep the party grouped on the leader tank
    // during any fight once the leash loosens (advanced/dynamic pull), so a healer
    // dragged out of LOS/range of the tank closes back in instead of standing idle
    // while the party dies. Relevance above the stock combat movers (MoveChase ~30)
    // so it owns the tick over a follower chasing a far target, but BELOW the camp
    // actions (assist 35, stay-at-camp / pull-maneuver 60) — those own positioning
    // during an advanced-pull camp, where this trigger stands down anyway. Inert
    // the instant the bot is back inside the leash / in LOS. See
    // DungeonClearRegroupCombatTrigger.
    triggers.push_back(new TriggerNode(
        "dungeon clear regroup combat",
        { NextAction("dungeon clear regroup combat", 33.0f) }));

    // Sunken Temple Avatar of Hakkar orchestration, COMBAT side — THE place these
    // run. The encounter is a continuous wave fight, so every member is in combat
    // almost the whole time; with the handlers only in the non-combat strategy
    // (their original home) the win path never executed — the flames stayed 0/4
    // and the WaitForSpawn(Avatar) step just timed out (the "very long fight").
    // Relevance ABOVE every stock combat mover/attack (MoveChase ~30) and the DC
    // camp/regroup rungs so the carrier actually peels mid-fight to silence a
    // resetting suppressor / douse a flame / grab blood. Same suppressor > flame >
    // loot-blood order as the non-combat copy (flame above loot so a blood carrier
    // douses instead of hoarding). All inert outside the live Sanctum.
    triggers.push_back(new TriggerNode(
        "dungeon clear hakkar suppressor",
        { NextAction("dungeon clear hakkar suppressor", 64.0f) }));
    triggers.push_back(new TriggerNode(
        "dungeon clear hakkar flame",
        { NextAction("dungeon clear hakkar flame", 63.0f) }));
    triggers.push_back(new TriggerNode(
        "dungeon clear hakkar loot blood",
        { NextAction("dungeon clear hakkar loot blood", 62.0f) }));
}
