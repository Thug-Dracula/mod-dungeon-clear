/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARSTRATEGY_H
#define _PLAYERBOT_DUNGEONCLEARSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

class DungeonClearStrategy : public Strategy
{
public:
    DungeonClearStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "dungeon clear"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Combat-engine companion to "dungeon clear". Holds ONLY the advanced-pull
// maneuver trigger — the in-combat half of the pull-to-camp, which can't live in
// the non-combat strategy because the bot runs its combat engine the instant it
// aggros. Resident on every bot's combat engine but inert unless that bot is the
// leader and mid-pull; carries no multipliers, so it never alters normal combat.
class DungeonClearCombatStrategy : public Strategy
{
public:
    DungeonClearCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "dungeon clear combat"; }
    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
