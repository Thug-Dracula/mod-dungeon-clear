/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/EventConditionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

namespace
{
    DungeonEventProgress Prog(uint32 eventId, uint32 stepIndex = 0, uint32 stepStartMs = 0)
    {
        DungeonEventProgress p;
        p.eventId = eventId;
        p.stepIndex = stepIndex;
        p.stepStartMs = stepStartMs;
        return p;
    }
}

// --- EventBuilder ---------------------------------------------------------

TEST(EventBuilderTest, BuildsTypedStepsInOrder)
{
    DungeonEvent e = EventBuilder(5, 9, "x")
                         .Anchored(7)
                         .MoveTo(1.0f, 2.0f, 3.0f, 7.0f)
                         .UseGO(100, 12.0f)
                         .WaitForGOState(100, 1, 8000)
                         .Build();

    ASSERT_EQ(e.steps.size(), 3u);
    EXPECT_EQ(e.mapId, 5u);
    EXPECT_EQ(e.id, 9u);
    EXPECT_EQ(e.orderIndex, 7u);
    EXPECT_TRUE(e.required);
    EXPECT_EQ(e.activation, EventActivation::Anchored);

    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(e.steps[0].radius, 7.0f);
    EXPECT_FLOAT_EQ(e.steps[0].x, 1.0f);

    EXPECT_EQ(e.steps[1].kind, EventStepKind::UseGameObject);
    EXPECT_EQ(e.steps[1].goEntry, 100u);
    EXPECT_FLOAT_EQ(e.steps[1].radius, 12.0f);

    EXPECT_EQ(e.steps[2].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(e.steps[2].wantState, 1u);
    EXPECT_EQ(e.steps[2].timeoutMs, 8000u);
}

TEST(EventBuilderTest, OptionalAndConditionalFlags)
{
    DungeonEvent e = EventBuilder(1, 1, "c").Conditional(42).Wait(500).Optional().Build();
    EXPECT_EQ(e.activation, EventActivation::Conditional);
    EXPECT_EQ(e.conditionId, 42u);
    EXPECT_FALSE(e.required);
    ASSERT_EQ(e.steps.size(), 1u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::Wait);
    EXPECT_EQ(e.steps[0].durationMs, 500u);
}

// --- DungeonEventExecutor::Advance (pure) ---------------------------------

TEST(DungeonEventAdvance, EmptyEventCompletes)
{
    DungeonEvent ev = EventBuilder(0, 1, "e").Build();
    DungeonEventProgress p = Prog(1);
    EXPECT_EQ(DungeonEventExecutor::Advance(ev, p, StepResult::Running, 1000, 30000),
              EventDriveOutcome::Completed);
}

TEST(DungeonEventAdvance, DoneWalksThroughStepsThenCompletes)
{
    DungeonEvent ev = EventBuilder(0, 1, "e").Wait(1000).Wait(1000).Build();
    DungeonEventProgress p = Prog(1, 0, 0);

    // First step Done -> still running, cursor advanced and step clock re-stamped.
    EXPECT_EQ(DungeonEventExecutor::Advance(ev, p, StepResult::Done, 500, 30000),
              EventDriveOutcome::Running);
    EXPECT_EQ(p.stepIndex, 1u);
    EXPECT_EQ(p.stepStartMs, 500u);

    // Last step Done -> whole event complete.
    EXPECT_EQ(DungeonEventExecutor::Advance(ev, p, StepResult::Done, 800, 30000),
              EventDriveOutcome::Completed);
    EXPECT_EQ(p.stepIndex, 2u);
}

TEST(DungeonEventAdvance, RunningHoldsBeforeTimeout)
{
    DungeonEvent ev = EventBuilder(0, 1, "e").WaitForSpawn(5, true, 5000).Build();
    DungeonEventProgress p = Prog(1, 0, 1000);
    EXPECT_EQ(DungeonEventExecutor::Advance(ev, p, StepResult::Running, 3000, 30000),
              EventDriveOutcome::Running);  // elapsed 2000 < 5000
    EXPECT_EQ(p.stepIndex, 0u);
}

TEST(DungeonEventAdvance, RunningTimesOutRequiredStallsOptionalSkips)
{
    DungeonEvent req = EventBuilder(0, 1, "e").WaitForSpawn(5, true, 5000).Build();
    DungeonEventProgress p1 = Prog(1, 0, 1000);
    EXPECT_EQ(DungeonEventExecutor::Advance(req, p1, StepResult::Running, 7000, 30000),
              EventDriveOutcome::Stalled);  // elapsed 6000 >= 5000

    DungeonEvent opt = EventBuilder(0, 1, "e").WaitForSpawn(5, true, 5000).Optional().Build();
    DungeonEventProgress p2 = Prog(1, 0, 1000);
    EXPECT_EQ(DungeonEventExecutor::Advance(opt, p2, StepResult::Running, 7000, 30000),
              EventDriveOutcome::Skipped);
}

TEST(DungeonEventAdvance, DefaultTimeoutUsedWhenStepHasNone)
{
    DungeonEvent ev = EventBuilder(0, 1, "e").Wait(1000).Build();  // step timeoutMs 0
    DungeonEventProgress p = Prog(1, 0, 0);
    EXPECT_EQ(DungeonEventExecutor::Advance(ev, p, StepResult::Running, 2000, 3000),
              EventDriveOutcome::Running);  // 2000 < default 3000
    EXPECT_EQ(DungeonEventExecutor::Advance(ev, p, StepResult::Running, 4000, 3000),
              EventDriveOutcome::Stalled);  // 4000 >= default 3000
}

TEST(DungeonEventAdvance, BlockedAlwaysStallsEvenWhenOptional)
{
    // Blocked means "needs the human", distinct from Failed — an optional event
    // does NOT silently skip a hard block.
    DungeonEvent ev = EventBuilder(0, 1, "e").Custom(1).Optional().Build();
    DungeonEventProgress p = Prog(1);
    EXPECT_EQ(DungeonEventExecutor::Advance(ev, p, StepResult::Blocked, 0, 30000),
              EventDriveOutcome::Stalled);
}

TEST(DungeonEventAdvance, FailedRequiredStallsOptionalSkips)
{
    DungeonEvent req = EventBuilder(0, 1, "e").Wait(1).Build();
    DungeonEventProgress p1 = Prog(1);
    EXPECT_EQ(DungeonEventExecutor::Advance(req, p1, StepResult::Failed, 0, 30000),
              EventDriveOutcome::Stalled);

    DungeonEvent opt = EventBuilder(0, 1, "e").Wait(1).Optional().Build();
    DungeonEventProgress p2 = Prog(1);
    EXPECT_EQ(DungeonEventExecutor::Advance(opt, p2, StepResult::Failed, 0, 30000),
              EventDriveOutcome::Skipped);
}

// --- DungeonEventRegistry (shipped table) ---------------------------------

TEST(DungeonEventRegistryTest, FindAndHasEvents)
{
    EXPECT_NE(DungeonEventRegistry::Find(109, 1), nullptr);  // Sunken Temple altar
    EXPECT_NE(DungeonEventRegistry::Find(209, 1), nullptr);  // ZulFarrak summit
    EXPECT_EQ(DungeonEventRegistry::Find(109, 2), nullptr);  // no such event
    EXPECT_EQ(DungeonEventRegistry::Find(0, 1), nullptr);    // no such map
    EXPECT_EQ(DungeonEventRegistry::Find(109, 0), nullptr);  // id 0 is "none"

    EXPECT_TRUE(DungeonEventRegistry::HasEvents(109));
    EXPECT_TRUE(DungeonEventRegistry::HasEvents(209));
    EXPECT_FALSE(DungeonEventRegistry::HasEvents(34));  // Stockades — none
}

// Sunken Temple altar event waits for the Avatar (8443) and is OPTIONAL so a
// non-firing summon skips rather than wedging the clear (matches `dc skip` note).
TEST(DungeonEventRegistryTest, SunkenTempleWaitsForAvatarOptional)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(109, 1);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->steps.size(), 1u);
    EXPECT_EQ(e->steps[0].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(e->steps[0].creatureEntry, 8443u);
    EXPECT_TRUE(e->steps[0].wantAlive);
    EXPECT_FALSE(e->required);
}

// ZulFarrak temple (Executioner / Bly's Band) event: a PERSISTENT anchored step
// list that runs the whole pyramid set-piece — kill the executioner, crack a cage
// (UseGO), survive the waves (wait for Sezz'ziz), descend to kill the temple
// bosses (engage steps), gossip Weegli (door) then Bly (fight), and kill Bly.
TEST(DungeonEventRegistryTest, ZulFarrakTempleEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(209, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_TRUE(e->persistent);
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 9u);

    // 1. kill the executioner (engage-driven), then crack a cage to start it.
    EXPECT_EQ(e->steps[0].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[0].creatureEntry, 7274u);
    EXPECT_TRUE(e->steps[0].engage);
    EXPECT_EQ(e->steps[1].kind, EventStepKind::UseGameObject);
    EXPECT_EQ(e->steps[1].goEntry, 141073u);

    // 2. survive the waves — wait for Sezz'ziz to spawn (wave 3).
    EXPECT_EQ(e->steps[2].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(e->steps[2].creatureEntry, 7275u);
    EXPECT_TRUE(e->steps[2].wantAlive);

    // 3. descend and kill the two temple bosses (engage-driven).
    EXPECT_EQ(e->steps[3].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[3].creatureEntry, 7796u);  // Nekrum
    EXPECT_TRUE(e->steps[3].engage);
    EXPECT_EQ(e->steps[4].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[4].creatureEntry, 7275u);  // Sezz'ziz
    EXPECT_TRUE(e->steps[4].engage);

    // 4. goblin FIRST (opens the door), then a short dwell before provoking Bly.
    EXPECT_EQ(e->steps[5].kind, EventStepKind::Gossip);
    EXPECT_EQ(e->steps[5].creatureEntry, 7607u);  // Weegli
    EXPECT_EQ(e->steps[6].kind, EventStepKind::Wait);
    EXPECT_GT(e->steps[6].durationMs, 0u);

    // 5. human starts the fight; killing Bly ends the event.
    EXPECT_EQ(e->steps[7].kind, EventStepKind::Gossip);
    EXPECT_EQ(e->steps[7].creatureEntry, 7604u);  // Bly
    EXPECT_EQ(e->steps[8].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[8].creatureEntry, 7604u);
    EXPECT_TRUE(e->steps[8].engage);
}

// The builder's KillCreatureEngage marks the engage flag (vs plain KillCreature
// which only gates), and Timeout() tunes the last-added step's timeout.
TEST(DungeonEventBuilderTest, KillCreatureEngageAndTimeout)
{
    DungeonEvent e = EventBuilder(1, 1, "e")
                         .KillCreature(100)
                         .KillCreatureEngage(200)
                         .WaitForSpawn(300, true).Timeout(900000)
                         .Build();
    ASSERT_EQ(e.steps.size(), 3u);
    EXPECT_FALSE(e.steps[0].engage);
    EXPECT_TRUE(e.steps[1].engage);
    EXPECT_EQ(e.steps[1].creatureEntry, 200u);
    EXPECT_EQ(e.steps[2].timeoutMs, 900000u);
}

// --- Milestone 2: conditional activation ----------------------------------

// Conditional() returns only EventActivation::Conditional events for the map.
// Sunken Temple (109) has an anchored event but no conditional one; Shadowfang
// Keep (33) has the conditional courtyard-door event.
TEST(DungeonEventConditional, ConditionalListFiltersByActivation)
{
    EXPECT_TRUE(DungeonEventRegistry::Conditional(109).empty());  // anchored only
    EXPECT_TRUE(DungeonEventRegistry::Conditional(209).empty());

    // SFK has two faction-specific conditional events (Alliance + Horde).
    std::vector<DungeonEvent const*> sfk = DungeonEventRegistry::Conditional(33);
    ASSERT_EQ(sfk.size(), 2u);
    EXPECT_EQ(sfk[0]->id, 1u);
    EXPECT_EQ(sfk[0]->conditionId, 1u);  // Alliance
    EXPECT_EQ(sfk[1]->id, 2u);
    EXPECT_EQ(sfk[1]->conditionId, 2u);  // Horde
    for (DungeonEvent const* e : sfk)
        EXPECT_EQ(e->activation, EventActivation::Conditional);
}

// Each SFK courtyard event: walk to the faction's cell lever, pull it (UseGO) to
// open the prison gate, gossip the freed prisoner (option 0), then wait for the
// Courtyard Door (18895) to open. Optional so a non-firing script degrades to the
// normal door-blocked stall instead of livelocking. Alliance frees Ashcrombe via
// lever 18901; Horde frees Adamant via lever 18900.
TEST(DungeonEventConditional, ShadowfangCourtyardEventShape)
{
    struct Faction { uint32 id; uint32 lever; uint32 prisoner; };
    for (Faction const& f : { Faction{1, 18901, 3850}, Faction{2, 18900, 3849} })
    {
        DungeonEvent const* e = DungeonEventRegistry::Find(33, f.id);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->activation, EventActivation::Conditional);
        EXPECT_FALSE(e->required);
        ASSERT_EQ(e->steps.size(), 5u);

        EXPECT_EQ(e->steps[0].kind, EventStepKind::MoveTo);  // to the lever

        EXPECT_EQ(e->steps[1].kind, EventStepKind::UseGameObject);
        EXPECT_EQ(e->steps[1].goEntry, f.lever);

        EXPECT_EQ(e->steps[2].kind, EventStepKind::Gossip);
        EXPECT_EQ(e->steps[2].creatureEntry, f.prisoner);
        EXPECT_EQ(e->steps[2].gossipOption, 0);

        EXPECT_EQ(e->steps[3].kind, EventStepKind::MoveTo);  // to the courtyard door

        EXPECT_EQ(e->steps[4].kind, EventStepKind::WaitForGameObjectState);
        EXPECT_EQ(e->steps[4].goEntry, 18895u);
        EXPECT_EQ(e->steps[4].wantState, 0u);  // GO_STATE_ACTIVE (open)
    }
}

// The synthetic latch key is pure, injective, and lives in a high range that
// can't collide with real creature/anchor entries.
TEST(DungeonEventConditional, ConditionalLatchKeyIsHighAndInjective)
{
    EXPECT_EQ(DungeonEventExecutor::ConditionalLatchKey(1),
              DungeonEventExecutor::ConditionalLatchKey(1));
    EXPECT_NE(DungeonEventExecutor::ConditionalLatchKey(1),
              DungeonEventExecutor::ConditionalLatchKey(2));
    EXPECT_GT(DungeonEventExecutor::ConditionalLatchKey(1), 1000000u);
}

// EventConditionRegistry dispatch guards: id 0 and unregistered ids are false;
// a null bot is false (so a mis-authored event simply never activates); the
// shipped SFK condition (1) is registered.
TEST(DungeonEventConditionRegistry, DispatchGuards)
{
    EXPECT_FALSE(EventConditionRegistry::Has(0));
    EXPECT_FALSE(EventConditionRegistry::Has(9999));
    EXPECT_TRUE(EventConditionRegistry::Has(1));   // SFK Alliance
    EXPECT_TRUE(EventConditionRegistry::Has(2));   // SFK Horde
    EXPECT_TRUE(EventConditionRegistry::Has(3));   // room-aggro pre-clear (M3)

    EXPECT_FALSE(EventConditionRegistry::Evaluate(0, nullptr, nullptr));
    EXPECT_FALSE(EventConditionRegistry::Evaluate(9999, nullptr, nullptr));
    EXPECT_FALSE(EventConditionRegistry::Evaluate(1, nullptr, nullptr));  // null bot
}

// --- Milestone 3: room-aggro pre-clear -----------------------------------

// The SM Cathedral (189) room-aggro pre-clear is a Conditional gate (condition 3)
// with a single KillCreature step in "room-trash mode" (creatureEntry 0). It is
// required (hold the boss pull until the room is clear).
TEST(DungeonEventRoomAggro, ScarletCathedralEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(189, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Conditional);
    EXPECT_EQ(e->conditionId, 3u);
    EXPECT_TRUE(e->required);
    ASSERT_EQ(e->steps.size(), 1u);
    EXPECT_EQ(e->steps[0].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[0].creatureEntry, 0u);  // room-trash mode
}

// IsRoomAggroPreClear distinguishes the room-trash gate from the step-driven
// SFK gossip events and from anchored objectives — only the lone-KillCreature(0)
// Conditional shape qualifies, so HasRoomAggroEvent flags only those maps.
TEST(DungeonEventRoomAggro, PredicateAndHasRoomAggroEvent)
{
    DungeonEvent const* cath = DungeonEventRegistry::Find(189, 1);
    DungeonEvent const* sfk = DungeonEventRegistry::Find(33, 1);   // gossip event
    DungeonEvent const* st = DungeonEventRegistry::Find(109, 1);   // anchored wait
    ASSERT_NE(cath, nullptr);
    ASSERT_NE(sfk, nullptr);
    ASSERT_NE(st, nullptr);

    EXPECT_TRUE(DungeonEventRegistry::IsRoomAggroPreClear(*cath));
    EXPECT_FALSE(DungeonEventRegistry::IsRoomAggroPreClear(*sfk));
    EXPECT_FALSE(DungeonEventRegistry::IsRoomAggroPreClear(*st));

    EXPECT_TRUE(DungeonEventRegistry::HasRoomAggroEvent(189));
    EXPECT_FALSE(DungeonEventRegistry::HasRoomAggroEvent(33));   // gossip only
    EXPECT_FALSE(DungeonEventRegistry::HasRoomAggroEvent(109));  // anchored only
    EXPECT_FALSE(DungeonEventRegistry::HasRoomAggroEvent(0));

    // A non-Conditional KillCreature(0) (e.g. a hypothetical anchored row) is NOT
    // a room-aggro pre-clear — the activation guard matters.
    DungeonEvent anchored = EventBuilder(1, 1, "x").Anchored(0).KillCreature(0).Build();
    EXPECT_FALSE(DungeonEventRegistry::IsRoomAggroPreClear(anchored));
}
