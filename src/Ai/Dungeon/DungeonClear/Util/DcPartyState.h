/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_PARTY_STATE_H
#define _DC_PARTY_STATE_H

#include <string>

class AiObjectContext;
class Player;

class DcPartyState
{
public:
    // The HP/mana percentages the between-pulls rest gate (IsPartyReady) holds
    // for. These default to mod-playerbots' own drink/eat stop thresholds
    // (AiPlayerbot.AlmostFullHealth / AiPlayerbot.HighMana): a stock bot only eats
    // back up to AlmostFullHealth and drinks back up to HighMana, then stops, so
    // we clamp the gate to those targets to keep it reachable by resting alone.
    //
    // When the run sets DungeonClear.RestHealthPct / RestManaPct (> 0) the group
    // overrides those targets for this run: the gate uses the override directly
    // (the matching DungeonClearNeeds{Eat,Drink} triggers make bots eat/drink up
    // to it, so even a target above the playerbots stop value is reachable). The
    // `bot` resolves the run owner for the override lookup; pass any member.
    // See the README's "mod-playerbots interaction" section.
    static float RestMinHpPct(Player* bot = nullptr);

    static float RestMinMpPct(Player* bot = nullptr);

    // Returns true when the party has caught up and recovered enough to pull again:
    //  - every living party member on the bot's map has HP% >= minHpPct,
    //  - every living mana-using party member has mana% >= minMpPct,
    //  - every living member is within maxSpread yards of the bot.
    // Dead members are not blocking — the party-died trigger handles them.
    // Callers pass RestMinHpPct()/RestMinMpPct() for the recovery thresholds.
    static bool IsPartyReady(Player* bot, float minHpPct, float minMpPct, float maxSpread);

    // Between-pulls gate: party HP/MP recovered (RestMinHpPct/RestMinMpPct) and
    // spread within DungeonClear.PartyMaxSpread — except while a pull maneuver is
    // actually HOLDING the party at camp (Forming/Advancing/Returning), where the
    // spread requirement is deliberately waived: the party then holds back at camp
    // while the tank scouts ahead, so a spread check measured against the tank
    // would never pass and the tank would stop pulling after the first camp.
    // While merely advancing between packs (Idle phase) the spread stays enforced
    // so the tank still stops to let the party catch up.
    //
    // `requireNoLoot` additionally fails the gate while the bot has a corpse to
    // walk to ("has available loot"): the trigger ladder wants that (never start
    // a pull over pending loot), but the advance action must NOT — it handles
    // loot separately in Execute behind a commit-timeout, and folding the loot
    // flag in there would defeat the timeout. One shared body for both sides so
    // they can never drift again (they were two copies, and had).
    static bool IsBetweenPullsReady(Player* bot, AiObjectContext* context, bool requireNoLoot);

    // Returns true if any LIVING bot party member on the bot's map (excluding
    // `bot` itself) currently has a corpse it intends to loot — in any phase,
    // walking in (has available loot) or within reach (can loot). Reads each
    // member's own loot values cross-context (same pattern as
    // DungeonClearPartyTankValue); real players (no PlayerbotAI) are skipped
    // since we neither drive nor wait on their looting. Lets the dungeon-clear
    // tank hold its advance after a pull until the whole party has finished
    // looting; the caller bounds the wait with a commit-timeout.
    static bool IsAnyPartyMemberLooting(Player* bot);

    // Builds a short, human-readable account of who the tank is waiting on to
    // become pull-ready, using the SAME thresholds IsPartyReady is called with
    // (so the description always matches the gate that actually holds the
    // advance). Lists each living on-map member that is too far, low on health,
    // or low on mana, with the limiting reason — e.g. "Bob (low HP), Alice (out
    // of range)". Caps the list so the addon line stays short; extra members
    // collapse to "+N more". Returns "" when the party is ready (nobody to wait
    // on). Used by DcStatusAction to fill the addon "resting" detail.
    static std::string DescribePartyNotReady(Player* bot,
                                             float minHpPct, float minMpPct,
                                             float maxSpread);

    // Names the living bot party members currently looting (walking to or
    // standing on a corpse), comma-joined and capped like DescribePartyNotReady.
    // Returns "" when only the tank itself is looting / nobody is. Used to fill
    // the addon "looting" detail so the player can see who is holding up the
    // advance.
    static std::string DescribePartyLooting(Player* bot);

};

#endif  // _DC_PARTY_STATE_H
