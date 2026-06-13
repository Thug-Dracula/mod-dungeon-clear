/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Edge-pinning unit tests for the Dynamic-pull governor decision core
// (DcPullDecision::DecidePull). The live planner routes every commit through
// this function, so these tests are the regression net for the governor's
// arbitration: the no-target grace, the in-combat / non-Idle lock, the same-pack
// UPGRADE-only latch + throttle, and the patrol-wait gate's three outcomes.

#include "gtest/gtest.h"
#include "DcPullDecision.h"

using DcPullDecision::DecidePull;
using DcPullDecision::PullObservation;
using DcPullDecision::PullVerdict;

namespace
{
    // A target-present, gates-passed, plain-Leeroy baseline. Individual tests
    // flip exactly the fields they exercise.
    PullObservation Base()
    {
        PullObservation o;
        o.hasTarget = true;
        o.inCombat = false;
        o.phaseIdle = true;
        o.hasStandingVerdict = false;
        o.verdictGraceExpired = false;
        o.sameTarget = false;
        o.currentlyAdvanced = false;
        o.recheckElapsed = true;
        o.advanced = false;
        o.patrolWaitEnabled = false;
        o.atCommitRange = false;
        o.patrolContended = false;
        o.patrolWaitExpired = false;
        return o;
    }
}

// ---- No-target branch ------------------------------------------------------

TEST(DcPullDecision, NoTargetNoStandingVerdictDropsToLeeroy)
{
    PullObservation o = Base();
    o.hasTarget = false;
    o.hasStandingVerdict = false;
    EXPECT_EQ(DecidePull(o), PullVerdict::DropToLeeroy);
}

TEST(DcPullDecision, NoTargetWithinGraceHolds)
{
    PullObservation o = Base();
    o.hasTarget = false;
    o.hasStandingVerdict = true;
    o.verdictGraceExpired = false;
    EXPECT_EQ(DecidePull(o), PullVerdict::HoldNoTarget);
}

TEST(DcPullDecision, NoTargetGraceExpiredDrops)
{
    PullObservation o = Base();
    o.hasTarget = false;
    o.hasStandingVerdict = true;
    o.verdictGraceExpired = true;
    EXPECT_EQ(DecidePull(o), PullVerdict::DropToLeeroy);
}

// ---- In-combat / non-Idle lock --------------------------------------------

TEST(DcPullDecision, InCombatIsNoOp)
{
    PullObservation o = Base();
    o.inCombat = true;
    o.advanced = true;  // would otherwise commit Advanced
    EXPECT_EQ(DecidePull(o), PullVerdict::NoOp);
}

TEST(DcPullDecision, NonIdlePhaseIsNoOp)
{
    PullObservation o = Base();
    o.phaseIdle = false;
    EXPECT_EQ(DecidePull(o), PullVerdict::NoOp);
}

// ---- Same-pack latch / throttle -------------------------------------------

TEST(DcPullDecision, SameTargetAlreadyAdvancedIsLocked)
{
    PullObservation o = Base();
    o.sameTarget = true;
    o.currentlyAdvanced = true;
    o.advanced = false;  // a downgrade must NOT happen — stays locked
    EXPECT_EQ(DecidePull(o), PullVerdict::NoOp);
}

TEST(DcPullDecision, SameTargetThrottledIsNoOp)
{
    PullObservation o = Base();
    o.sameTarget = true;
    o.currentlyAdvanced = false;
    o.recheckElapsed = false;  // throttle window not yet passed
    o.advanced = true;
    EXPECT_EQ(DecidePull(o), PullVerdict::NoOp);
}

TEST(DcPullDecision, SameTargetThrottleElapsedUpgradesToAdvanced)
{
    PullObservation o = Base();
    o.sameTarget = true;
    o.currentlyAdvanced = false;
    o.recheckElapsed = true;
    o.advanced = true;
    EXPECT_EQ(DecidePull(o), PullVerdict::Advanced);
}

// ---- Plain commit ----------------------------------------------------------

TEST(DcPullDecision, PlainLeeroy)
{
    PullObservation o = Base();
    o.advanced = false;
    EXPECT_EQ(DecidePull(o), PullVerdict::Leeroy);
}

TEST(DcPullDecision, PlainAdvancedWithPatrolWaitOff)
{
    PullObservation o = Base();
    o.advanced = true;
    o.patrolWaitEnabled = false;
    EXPECT_EQ(DecidePull(o), PullVerdict::Advanced);
}

// ---- Patrol-wait gate (the three outcomes) --------------------------------

TEST(DcPullDecision, PatrolContendedApproachingWalksInAsLeeroy)
{
    PullObservation o = Base();
    o.advanced = true;
    o.patrolWaitEnabled = true;
    o.atCommitRange = false;     // still approaching
    o.patrolContended = true;
    EXPECT_EQ(DecidePull(o), PullVerdict::ApproachAsLeeroy);
}

TEST(DcPullDecision, PatrolContendedAtCommitHolds)
{
    PullObservation o = Base();
    o.advanced = true;
    o.patrolWaitEnabled = true;
    o.atCommitRange = true;
    o.patrolContended = true;
    o.patrolWaitExpired = false;  // still within the wait budget
    EXPECT_EQ(DecidePull(o), PullVerdict::PatrolWaitHold);
}

TEST(DcPullDecision, PatrolContendedAtCommitWaitExpiredCommits)
{
    PullObservation o = Base();
    o.advanced = true;
    o.patrolWaitEnabled = true;
    o.atCommitRange = true;
    o.patrolContended = true;
    o.patrolWaitExpired = true;   // budget ran out — proceed with the heavy verdict
    EXPECT_EQ(DecidePull(o), PullVerdict::Advanced);
}

TEST(DcPullDecision, AdvancedAtCommitNotContendedCommits)
{
    PullObservation o = Base();
    o.advanced = true;
    o.patrolWaitEnabled = true;
    o.atCommitRange = true;
    o.patrolContended = false;    // genuine heavy pull, not a lone patrol
    EXPECT_EQ(DecidePull(o), PullVerdict::Advanced);
}

TEST(DcPullDecision, AdvancedApproachingNotContendedCommits)
{
    PullObservation o = Base();
    o.advanced = true;
    o.patrolWaitEnabled = true;
    o.atCommitRange = false;
    o.patrolContended = false;
    EXPECT_EQ(DecidePull(o), PullVerdict::Advanced);
}
