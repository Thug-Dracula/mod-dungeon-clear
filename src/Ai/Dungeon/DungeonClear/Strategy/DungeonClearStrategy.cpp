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

    // Within engage range of next boss.
    triggers.push_back(new TriggerNode(
        "dungeon clear at boss",
        { NextAction("dungeon clear engage boss", 30.0f) }));

    // Blocking trash on the path to the next boss.
    triggers.push_back(new TriggerNode(
        "dungeon clear blocking trash",
        { NextAction("dungeon clear engage trash", 25.0f) }));

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
    triggers.push_back(new TriggerNode("dc status",         { NextAction("dc status", chatRel) }));
    triggers.push_back(new TriggerNode("dc bosses",         { NextAction("dc bosses", chatRel) }));
}

void DungeonClearStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonClearMultiplier(botAI));
}
