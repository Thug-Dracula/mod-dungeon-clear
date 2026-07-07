/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMultiplier.h"

#include "Action.h"
#include "FollowActions.h"
#include "Player.h"
#include "Playerbots.h"
#include "Position.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

float DungeonClearMultiplier::GetValue(Action* action)
{
    if (!action || !botAI || !bot)
        return 1.0f;

    std::string const& name = action->getName();

    // Rest-target cap. Applies to EVERY bot in an active DC run — the leader tank
    // AND its followers — so the whole group stops eating/drinking at the group's
    // chosen target (DungeonClear.RestHealthPct / RestManaPct). Followers never
    // set `enabled`, so we gate on the cross-bot "dungeon clear party tank" value
    // (non-null only while the leader's clear runs and is unpaused) rather than
    // the per-bot enabled flag used below. The matching
    // DungeonClearNeeds{Eat,Drink} triggers raise the floor (drink/eat up to the
    // target even above the stock playerbots stop); this caps the ceiling so a
    // target BELOW the stock stop is honoured too. 0 = inherit, no cap.
    if (name == "food" || name == "drink")
    {
        if (AI_VALUE(Player*, DcKey::PartyTank))
        {
            bool const isDrink = (name == "drink");
            uint32 const target =
                DcSettings::GetUInt(bot, isDrink ? "RestManaPct" : "RestHealthPct");
            if (target > 0)
            {
                float const pct =
                    isDrink ? bot->GetPowerPct(POWER_MANA) : bot->GetHealthPct();
                if (pct >= static_cast<float>(target))
                    return 0.0f;
            }
        }
    }

    // The stock anti-stack shuffle ("move out of collision", NonCombatStrategy
    // relevance 2) fights DC's positioning and can livelock a follower: the
    // follow-tank/camp-hold pin is a FIXED point, so when another unit stands
    // inside ContactDistance the shuffle hops the bot ~followDistance away in a
    // random direction, the persistent MoveFollow generator walks it straight
    // back onto the pin, and the two alternate forever — a rapid two-steps-out/
    // two-steps-back dance. The bot never stands still long enough to sit, so
    // it can't drink/eat, and the leader's between-pulls rest gate then stalls
    // the whole run on its mana. DC's own positioning decides where everyone
    // stands; drop the shuffle for every member of an active run (same
    // cross-bot gate as the rest cap above).
    if (name == "move out of collision" &&
        AI_VALUE(Player*, DcKey::PartyTank))
        return 0.0f;

    // Wander-style autonomous navigation (grind / rpg / travel). This is what
    // drives the bot around the world on its own.
    bool const isWander =
        name.find("grind") != std::string::npos ||
        name.find("rpg") != std::string::npos ||
        name == "travel" || name.find("travel ") != std::string::npos;

    // Proactive engagement: the bot's OWN target-picking that walks it to a mob
    // and pulls (the grind strategy fires "attack anything"; the pull strategy
    // fires "pull start"/"pull action"/"reach pull"). These run in the non-combat
    // engine BEFORE combat starts, so they bypass DC's triggers entirely — which
    // is exactly how a paused/parked tank still strolls THROUGH a navmesh-passable
    // door to a pack on the far side (the pack only aggros once the tank arrives,
    // so it never shows as combat). DC drives all engagement through its own
    // engage-trash/engage-boss actions, so the stock pickers must stay suppressed
    // whenever DC owns the bot — active OR paused. Reactive defense (a mob that
    // actually aggros the bot) is combat-engine and untouched here.
    bool const isProactiveEngage =
        name == "attack anything" ||
        name == "move random" ||
        name == "pull action" || name == "pull start" || name == "reach pull";

    // FOLLOWER in advanced-pull camp-hold. Followers never set `enabled`, so this
    // sits above the enabled gate below. In pull mode the party holds and
    // leapfrogs camp-to-camp (DungeonClearHoldAtCampAction); when it is parked the
    // hold action YIELDS the tick so the bot can rest/loot at camp — but that
    // would also let stock follow-master / wander / self-pull drag it off toward
    // the scouting tank. Suppress exactly those for a camp-held follower; food /
    // drink / loot / reactive combat fall through untouched so the party still
    // recovers. Only resolve the leader for an action we might actually suppress,
    // so the per-tick leader lookup stays off the hot path.
    if (isWander || isProactiveEngage || dynamic_cast<FollowAction*>(action))
    {
        Position camp;
        bool passive = false;
        if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
            return 0.0f;
    }

    bool const enabled = AI_VALUE(bool, DcKey::Enabled);
    bool const paused = AI_VALUE(bool, DcKey::Paused);

    // DC off: fully stock behavior, nothing suppressed.
    if (!enabled)
        return 1.0f;

    // Paused is a HOLD, not a hand-back to stock AI. This used to return 1.0
    // (full stock), which let the tank grind/pull off on its own — that's how a
    // paused tank walked straight THROUGH a door the player paused at to deal
    // with. Keep autonomous wandering AND proactive engagement suppressed so a
    // paused tank simply holds and follows the party like any member (follow is
    // left at 1.0, unlike the active branch below) until the player resumes.
    if (paused)
        return (isWander || isProactiveEngage) ? 0.0f : 1.0f;

    // --- Active (enabled && !paused) ------------------------------------------
    // The tank leads the clear — it must never follow its master. When Advance
    // yields to wait for the party to catch up (party spread > DungeonClear.PartyMaxSpread)
    // it StopMoving()s and parks; without this, the stock FollowAction (relevance
    // 1.0) then wins the idle tick and walks the tank BACK toward the stationary
    // player, who is now in range again, so next tick Advance runs it forward to
    // the spread limit and it yields again — a rubberband between the spread limit
    // and the player. Suppressing follow for the tank lets it simply hold at the
    // spread limit until the player catches up. Followers are unaffected: their
    // redirect (DungeonClearFollowTankAction) is a separate MovementAction, and
    // only non-tanks run it.
    if (PlayerbotAI::IsTank(bot) && dynamic_cast<FollowAction*>(action))
        return 0.0f;

    // Suppress wander AND the stock proactive-engagement pickers while DC is
    // active — DC owns engagement via its own engage-trash/engage-boss actions.
    // Anything else (loot, food, drink, reactive combat, our own dungeon-clear
    // actions, and follow for non-tanks) is untouched.
    if (isWander || isProactiveEngage)
        return 0.0f;
    return 1.0f;
}
