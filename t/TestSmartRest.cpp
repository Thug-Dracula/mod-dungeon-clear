/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcSmartRestDecision.h"

using DcSmartRestDecision::Decide;
using DcSmartRestDecision::Inputs;
using DcSmartRestDecision::Member;
using DcSmartRestDecision::Result;
using DcSmartRestDecision::kHumanReleaseMarginPct;
using DcSmartRestDecision::kReleasePct;

namespace
{
    // A "healthy" five-member party at full everything, default triggers
    // (hp 50 / DPS mana 10 / healer mana 40). Individual tests flip one thing.
    // Roster: [0] tank (mana user — prot paladin shape), [1] healer,
    // [2] caster DPS, [3] melee DPS (no mana), [4] human caster DPS.
    std::vector<Member> BaseParty()
    {
        Member tank;   tank.isManaUser = true;
        Member healer; healer.isManaUser = true; healer.isHealer = true;
        Member caster; caster.isManaUser = true;
        Member melee;  // no mana
        Member human;  human.isManaUser = true; human.isBot = false;
        return {tank, healer, caster, melee, human};
    }

    Inputs BaseInputs()
    {
        Inputs in;
        in.latched = false;
        in.restElapsedMs = 0;
        in.rearmed = true;
        in.hpTriggerPct = 50.0f;
        in.dpsManaTriggerPct = 10.0f;
        in.healerManaTriggerPct = 40.0f;
        in.maxRestMs = 180000;
        return in;
    }

    Inputs LatchedInputs()
    {
        Inputs in = BaseInputs();
        in.latched = true;
        in.restElapsedMs = 1000;
        return in;
    }
}

// ---- entering the latch ---------------------------------------------------------

TEST(DcSmartRestTest, FullPartyDoesNotLatch)
{
    Result const r = Decide(BaseInputs(), BaseParty());
    EXPECT_FALSE(r.latched);
    EXPECT_TRUE(r.blockers.empty());
}

TEST(DcSmartRestTest, DpsManaBelowTriggerLatches)
{
    auto party = BaseParty();
    party[2].manaPct = 9.0f;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_TRUE(r.latched);
    ASSERT_EQ(r.blockers.size(), 1u);
    EXPECT_EQ(r.blockers[0], 2u);
}

TEST(DcSmartRestTest, DpsManaJustAboveTriggerDoesNotLatch)
{
    auto party = BaseParty();
    party[2].manaPct = 11.0f;
    EXPECT_FALSE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, HealerManaBelowHealerTriggerLatches)
{
    auto party = BaseParty();
    party[1].manaPct = 39.0f;
    EXPECT_TRUE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, HealerManaJustAboveTriggerDoesNotLatch)
{
    auto party = BaseParty();
    party[1].manaPct = 41.0f;
    EXPECT_FALSE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, HealerTriggerDoesNotApplyToDps)
{
    // 39% mana would latch a healer, but a DPS pushes on down to 10%.
    auto party = BaseParty();
    party[2].manaPct = 39.0f;
    EXPECT_FALSE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, TankManaUsesDpsTrigger)
{
    auto party = BaseParty();
    party[0].manaPct = 9.0f;   // tank shares the DPS trigger
    EXPECT_TRUE(Decide(BaseInputs(), party).latched);
    party[0].manaPct = 39.0f;  // healer trigger must NOT catch the tank
    EXPECT_FALSE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, LowHpLatchesAnyRole)
{
    auto party = BaseParty();
    party[3].hpPct = 49.0f;    // the manaless melee
    EXPECT_TRUE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, HumanBelowTriggerLatches)
{
    auto party = BaseParty();
    party[4].manaPct = 5.0f;
    EXPECT_TRUE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, MeleeLowManaFieldIgnored)
{
    // A non-mana-user's manaPct is meaningless and must never latch.
    auto party = BaseParty();
    party[3].manaPct = 0.0f;
    EXPECT_FALSE(Decide(BaseInputs(), party).latched);
}

TEST(DcSmartRestTest, ZeroTriggerDisablesDimension)
{
    Inputs in = BaseInputs();
    in.hpTriggerPct = 0.0f;
    auto party = BaseParty();
    party[3].hpPct = 5.0f;
    EXPECT_FALSE(Decide(in, party).latched);

    in = BaseInputs();
    in.dpsManaTriggerPct = 0.0f;
    party = BaseParty();
    party[2].manaPct = 1.0f;
    EXPECT_FALSE(Decide(in, party).latched);

    in = BaseInputs();
    in.healerManaTriggerPct = 0.0f;
    party = BaseParty();
    party[1].manaPct = 1.0f;
    EXPECT_FALSE(Decide(in, party).latched);
}

TEST(DcSmartRestTest, AllTriggersZeroNeverLatches)
{
    Inputs in = BaseInputs();
    in.hpTriggerPct = 0.0f;
    in.dpsManaTriggerPct = 0.0f;
    in.healerManaTriggerPct = 0.0f;
    auto party = BaseParty();
    for (Member& m : party)
    {
        m.hpPct = 1.0f;
        m.manaPct = 1.0f;
    }
    EXPECT_FALSE(Decide(in, party).latched);
}

TEST(DcSmartRestTest, NotRearmedBlocksLatch)
{
    auto party = BaseParty();
    party[2].manaPct = 5.0f;
    Inputs in = BaseInputs();
    in.rearmed = false;
    EXPECT_FALSE(Decide(in, party).latched);
    in.rearmed = true;
    EXPECT_TRUE(Decide(in, party).latched);
}

// ---- holding / releasing the latch ----------------------------------------------

TEST(DcSmartRestTest, LatchedHoldsUntilBotsFull)
{
    auto party = BaseParty();
    party[2].manaPct = 98.0f;  // still drinking
    Result const r = Decide(LatchedInputs(), party);
    EXPECT_TRUE(r.latched);
    ASSERT_EQ(r.blockers.size(), 1u);
    EXPECT_EQ(r.blockers[0], 2u);
}

TEST(DcSmartRestTest, ReleaseEpsilon)
{
    auto party = BaseParty();
    for (Member& m : party)
    {
        m.hpPct = kReleasePct;
        m.manaPct = kReleasePct;
    }
    EXPECT_FALSE(Decide(LatchedInputs(), party).latched);  // 99.5 releases

    party[2].manaPct = 99.4f;
    EXPECT_TRUE(Decide(LatchedInputs(), party).latched);   // 99.4 holds

    party[2].manaPct = 100.0f;
    EXPECT_FALSE(Decide(LatchedInputs(), party).latched);  // 100.0 releases
}

TEST(DcSmartRestTest, LatchedHoldsOnBotHpToo)
{
    auto party = BaseParty();
    party[3].hpPct = 90.0f;    // above the 50 trigger but short of full
    EXPECT_TRUE(Decide(LatchedInputs(), party).latched);
}

TEST(DcSmartRestTest, ReleaseCannotInstantlyRelatch)
{
    // Hysteresis proof: the release state (everything at the release bar) is
    // strictly above every trigger, so an immediate re-eval stays unlatched.
    auto party = BaseParty();
    for (Member& m : party)
    {
        m.hpPct = kReleasePct;
        m.manaPct = kReleasePct;
    }
    ASSERT_FALSE(Decide(LatchedInputs(), party).latched);
    EXPECT_FALSE(Decide(BaseInputs(), party).latched);
}

// ---- humans: trigger + margin, never 100% ---------------------------------------

TEST(DcSmartRestTest, HumanHoldsOnlyToTriggerPlusMargin)
{
    auto party = BaseParty();
    // All bots full; the human sits below its release bar (10 + 5 = 15).
    party[4].manaPct = 12.0f;
    Result const held = Decide(LatchedInputs(), party);
    EXPECT_TRUE(held.latched);
    ASSERT_EQ(held.blockers.size(), 1u);
    EXPECT_EQ(held.blockers[0], 4u);

    party[4].manaPct = 16.0f;  // past the bar — releases well below 100%
    EXPECT_FALSE(Decide(LatchedInputs(), party).latched);
}

TEST(DcSmartRestTest, HumanHpBarIsTriggerPlusMargin)
{
    auto party = BaseParty();
    party[4].hpPct = 52.0f;    // bar = 50 + 5 = 55
    EXPECT_TRUE(Decide(LatchedInputs(), party).latched);
    party[4].hpPct = 56.0f;
    EXPECT_FALSE(Decide(LatchedInputs(), party).latched);
}

TEST(DcSmartRestTest, HumanOwesNothingOnDisabledDimension)
{
    Inputs in = LatchedInputs();
    in.hpTriggerPct = 0.0f;
    auto party = BaseParty();
    party[4].hpPct = 3.0f;     // hp trigger disabled — no hp bar for humans
    EXPECT_FALSE(Decide(in, party).latched);
}

TEST(DcSmartRestTest, BotsStillRestToFullOnDisabledDimension)
{
    // A full rest is a full rest: even with the mana trigger disabled, a bot
    // mid-drink holds the latch until full.
    Inputs in = LatchedInputs();
    in.dpsManaTriggerPct = 0.0f;
    auto party = BaseParty();
    party[2].manaPct = 80.0f;
    EXPECT_TRUE(Decide(in, party).latched);
}

// ---- timeout failsafe -----------------------------------------------------------

TEST(DcSmartRestTest, TimeoutReleases)
{
    auto party = BaseParty();
    party[4].manaPct = 5.0f;   // AFK human never drinks
    Inputs in = LatchedInputs();
    in.restElapsedMs = in.maxRestMs;
    Result const r = Decide(in, party);
    EXPECT_FALSE(r.latched);
    EXPECT_TRUE(r.timedOut);
}

TEST(DcSmartRestTest, ZeroMaxRestNeverTimesOut)
{
    auto party = BaseParty();
    party[4].manaPct = 5.0f;
    Inputs in = LatchedInputs();
    in.maxRestMs = 0;
    in.restElapsedMs = 0xFFFFFFF0u;
    Result const r = Decide(in, party);
    EXPECT_TRUE(r.latched);
    EXPECT_FALSE(r.timedOut);
}

TEST(DcSmartRestTest, NaturalReleaseIsNotTimedOut)
{
    Result const r = Decide(LatchedInputs(), BaseParty());
    EXPECT_FALSE(r.latched);
    EXPECT_FALSE(r.timedOut);
}

// ---- degenerates ----------------------------------------------------------------

TEST(DcSmartRestTest, EmptyPartyNeverLatches)
{
    EXPECT_FALSE(Decide(BaseInputs(), {}).latched);
    // A latched run whose snapshot went empty (everyone died/left) releases.
    EXPECT_FALSE(Decide(LatchedInputs(), {}).latched);
}

TEST(DcSmartRestTest, SoloTankLatchesOnItself)
{
    Member solo;
    solo.isManaUser = true;
    solo.manaPct = 5.0f;
    Result const r = Decide(BaseInputs(), {solo});
    EXPECT_TRUE(r.latched);
    ASSERT_EQ(r.blockers.size(), 1u);
    EXPECT_EQ(r.blockers[0], 0u);
}

TEST(DcSmartRestTest, MixedRolesBlockersNameOnlyTheTriggering)
{
    // Healer at 35% is below ITS trigger; a DPS at 35% is not.
    auto party = BaseParty();
    party[1].manaPct = 35.0f;
    party[2].manaPct = 35.0f;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_TRUE(r.latched);
    ASSERT_EQ(r.blockers.size(), 1u);
    EXPECT_EQ(r.blockers[0], 1u);
}
