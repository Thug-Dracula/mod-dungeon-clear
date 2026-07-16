/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSMARTRESTDECISION_H
#define _PLAYERBOT_DCSMARTRESTDECISION_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Pure decision kernel for Smart Rest — the hysteresis rest latch. The legacy
// rest gate is a single threshold used for BOTH ends (the party stops whenever
// anyone is below RestHealthPct/RestManaPct and rests back up to that same
// value), which produces constant micro-rests. Smart Rest splits the two ends:
// a LOW, role-based trigger (DPS/tank mana, healer mana, any-role HP) latches a
// party-wide rest, and the release bar is FULL health and mana — fewer, longer
// rests. Between rests eating/drinking is fully suppressed (the multiplier's
// job, keyed off this latch).
//
// Extracted engine-free so it is unit-testable in isolation, mirroring
// DecidePull / DecideCombatRegroup. DcSmartRest (the glue) gathers the live
// group into Member snapshots, calls Decide against the latch stored in the
// leader's DcRunState, and writes the verdict back. Nothing here touches a
// Player/Unit/context, so no game headers are needed.

namespace DcSmartRestDecision
{
    // Release bar for BOT members: "full", float-safe. GetHealthPct/GetPowerPct
    // return 100.0f exactly when full (cur/max with cur==max), but a buff or
    // aura shifting the max pool mid-rest could strand a bot at 99.x forever —
    // half a percent of slack costs nothing perceptible.
    constexpr float kReleasePct = 99.5f;

    // Release bar for HUMAN members: their role trigger plus this margin. A
    // human can't be forced to drink, so demanding kReleasePct of them would
    // deadlock the latch; the margin also guarantees a release can never
    // instantly re-latch on the same human sitting at the trigger edge.
    constexpr float kHumanReleaseMarginPct = 5.0f;

    // One living, same-map group member, snapshotted by the glue. Dead and
    // off-map members are the snapshot builder's job to exclude, not ours.
    struct Member
    {
        float hpPct = 100.0f;
        float manaPct = 100.0f;   // meaningful only when isManaUser
        bool  isManaUser = false; // powerType == POWER_MANA && maxMana > 0
        bool  isHealer = false;   // PlayerbotAI::IsHeal — selects the mana trigger
        bool  isBot = true;       // GET_PLAYERBOT_AI(member) != nullptr
    };

    struct Inputs
    {
        bool          latched = false;     // stored latch (DcRunState::smartRestLatched)
        std::uint32_t restElapsedMs = 0;   // now - smartRestSinceMs; 0 when not latched
        bool          rearmed = true;      // false during the post-timeout cooldown
        float         hpTriggerPct = 50.0f;       // SmartRestHealthPct (all roles)
        float         dpsManaTriggerPct = 10.0f;  // SmartRestDpsManaPct (DPS + tanks)
        float         healerManaTriggerPct = 40.0f;  // SmartRestHealerManaPct
        std::uint32_t maxRestMs = 0;       // timeout failsafe; 0 = never time out
    };

    struct Result
    {
        bool latched = false;
        bool timedOut = false;             // released by the failsafe this eval
        std::vector<std::size_t> blockers; // members below trigger (entering) or
                                           // below their release bar (holding)
    };

    // The member's mana trigger for its role. 0 = that dimension disabled.
    float ManaTriggerPct(Member const& m, Inputs const& in);

    // Release bars for one member: bots rest to kReleasePct on every applicable
    // dimension (a full rest is a full rest, even on a dimension whose trigger
    // is disabled); humans only owe trigger + margin, and owe nothing on a
    // disabled dimension (bar 0 — any value passes). Mana bar is 0 for
    // non-mana users.
    float HpReleaseBar(Member const& m, Inputs const& in);
    float ManaReleaseBar(Member const& m, Inputs const& in);

    // The two halves of the hysteresis, exposed so the glue's DescribeWait can
    // name blockers with the exact same rules Decide applies.
    bool BelowTrigger(Member const& m, Inputs const& in);
    bool BelowRelease(Member const& m, Inputs const& in);

    // The verdict:
    //   Not latched: latch when rearmed and ANY member is BelowTrigger.
    //   Latched:     time out when maxRestMs > 0 and restElapsedMs >= maxRestMs;
    //                otherwise release only when NO member is BelowRelease.
    // Empty member list never latches.
    Result Decide(Inputs const& in, std::vector<Member> const& members);
}

#endif  // _PLAYERBOT_DCSMARTRESTDECISION_H
