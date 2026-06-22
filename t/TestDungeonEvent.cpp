/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/EventConditionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
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

TEST(EventBuilderTest, JumpStepCarriesLandingAndRadius)
{
    // A drop-down event: walk onto the lip, then jump the off-mesh gap onto the
    // landing shelf (Wailing Caverns → Lord Serpentis).
    DungeonEvent e = EventBuilder(43, 1, "Drop to Lord Serpentis")
                         .Anchored(3)
                         .MoveTo(-100.0f, 10.0f, -20.0f, 3.0f)
                         .Jump(-120.0f, -24.0f, -28.0f, 5.0f)
                         .Build();

    ASSERT_EQ(e.steps.size(), 2u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);

    EventStep const& j = e.steps[1];
    EXPECT_EQ(j.kind, EventStepKind::Jump);
    EXPECT_FLOAT_EQ(j.x, -120.0f);
    EXPECT_FLOAT_EQ(j.y, -24.0f);
    EXPECT_FLOAT_EQ(j.z, -28.0f);
    EXPECT_FLOAT_EQ(j.radius, 5.0f);
}

TEST(EventBuilderTest, JumpStepDefaultRadius)
{
    DungeonEvent e = EventBuilder(43, 2, "j").Jump(1.0f, 2.0f, 3.0f).Build();
    ASSERT_EQ(e.steps.size(), 1u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::Jump);
    EXPECT_FLOAT_EQ(e.steps[0].radius, 4.0f);
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
    EXPECT_NE(DungeonEventRegistry::Find(109, 1), nullptr);  // Sunken Temple forcefield
    EXPECT_NE(DungeonEventRegistry::Find(209, 1), nullptr);  // ZulFarrak summit
    EXPECT_NE(DungeonEventRegistry::Find(209, 2), nullptr);  // ZulFarrak Gahz'rilla
    EXPECT_NE(DungeonEventRegistry::Find(230, 1), nullptr);  // BRD Ring of Law
    EXPECT_NE(DungeonEventRegistry::Find(109, 11), nullptr);  // ST Atal'alarion
    EXPECT_EQ(DungeonEventRegistry::Find(109, 99), nullptr);  // no such event
    EXPECT_EQ(DungeonEventRegistry::Find(0, 1), nullptr);    // no such map
    EXPECT_EQ(DungeonEventRegistry::Find(109, 0), nullptr);  // id 0 is "none"

    EXPECT_TRUE(DungeonEventRegistry::HasEvents(109));
    EXPECT_TRUE(DungeonEventRegistry::HasEvents(209));
    EXPECT_FALSE(DungeonEventRegistry::HasEvents(34));  // Stockades — none
}

// Sunken Temple GATE 1: the forcefield is dropped via SIX Anchored ring-anchor
// events (ids 1/12/13/14/15/16), one per Atal'ai defender, each engage-killing
// exactly ONE defender. Anchored (not conditional) because each defender sits on
// its own balcony only boss-nav's LongRangePathfinder can reach — a raw MoveTo
// walks the tank to the wrong floor-level room. Pairing two per anchor skipped
// the second (far) defender, the reported bug. The kill step has a generous
// per-step timeout (mini-boss walk + fight).
TEST(DungeonEventRegistryTest, SunkenTempleForcefieldRingAnchors)
{
    struct Anchor { uint32 id; uint32 defender; };
    for (Anchor const& a : { Anchor{1, 5717},    // Mijan
                             Anchor{12, 5716},   // Zul'Lor
                             Anchor{13, 5712},   // Zolo
                             Anchor{14, 5713},   // Gasher
                             Anchor{15, 5714},   // Loro
                             Anchor{16, 5715} }) // Hukku
    {
        DungeonEvent const* e = DungeonEventRegistry::Find(109, a.id);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->activation, EventActivation::Anchored);
        EXPECT_TRUE(e->required);
        ASSERT_EQ(e->steps.size(), 1u);
        EXPECT_EQ(e->steps[0].kind, EventStepKind::KillCreature);
        EXPECT_TRUE(e->steps[0].engage);
        EXPECT_EQ(e->steps[0].creatureEntry, a.defender);
        EXPECT_EQ(e->steps[0].timeoutMs, 120000u);
    }
}

// Sunken Temple GATE 2: events 2-7 are the six statue clicks, one Anchored
// (Optional) UseGO each, in entry/click order 148830..148835.
TEST(DungeonEventRegistryTest, SunkenTempleStatueClicks)
{
    uint32 const statues[6] = {148830, 148831, 148832, 148833, 148834, 148835};
    for (uint32 i = 0; i < 6; ++i)
    {
        DungeonEvent const* e = DungeonEventRegistry::Find(109, 2 + i);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->activation, EventActivation::Anchored);
        EXPECT_FALSE(e->required);  // optional pit wing
        ASSERT_EQ(e->steps.size(), 1u);
        EXPECT_EQ(e->steps[0].kind, EventStepKind::UseGameObject);
        EXPECT_EQ(e->steps[0].goEntry, statues[i]);
    }
}

// Sunken Temple GATE 3 + reordered bosses: the summon (8), Avatar (9),
// Weaver & Dreamscythe (10), and Atal'alarion (11) events.
TEST(DungeonEventRegistryTest, SunkenTempleIdolAvatarAndReorderedBosses)
{
    // Event 8 — Awaken the Soulflayer (optional pit-wing beat 1). The encounter
    // starts only by USING the Egg of Hakkar (10465) at the Sanctum centre — the
    // idol click is a QUESTGIVER no-op and a bare spell cast is rejected. The egg
    // summon FAILS unless used from dead centre and the objective arrive radius
    // lets boss-nav park the tank short, so a tight MoveTo precedes the UseItem to
    // walk the tank precisely to centre first (egg-positioning fix 2026-06-15).
    DungeonEvent const* idol = DungeonEventRegistry::Find(109, 8);
    ASSERT_NE(idol, nullptr);
    EXPECT_EQ(idol->activation, EventActivation::Anchored);
    EXPECT_FALSE(idol->required);
    ASSERT_EQ(idol->steps.size(), 2u);
    EXPECT_EQ(idol->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_EQ(idol->steps[1].kind, EventStepKind::UseItem);
    EXPECT_EQ(idol->steps[1].itemId, 10465u);

    // Event 9 — the Avatar fight: Persistent + Optional. The summon cast spawns
    // the Shade of Hakkar, which transforms into the Avatar (8443) ON ITS OWN
    // once the Nightmare Suppressors drive its counter to 25 (~79s). The bots
    // must NOT engage the suppressors (combat stops their counter tick), so the
    // event has NO channeler pre-kill steps — it only waits for the Avatar to
    // manifest, then kills it.
    DungeonEvent const* avatar = DungeonEventRegistry::Find(109, 9);
    ASSERT_NE(avatar, nullptr);
    EXPECT_EQ(avatar->activation, EventActivation::Anchored);
    EXPECT_TRUE(avatar->persistent);
    EXPECT_FALSE(avatar->required);
    ASSERT_EQ(avatar->steps.size(), 2u);
    EXPECT_EQ(avatar->steps[0].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(avatar->steps[0].creatureEntry, 8443u);  // Avatar of Hakkar
    EXPECT_GE(avatar->steps[0].timeoutMs, 120000u);    // long enough to manifest
    EXPECT_EQ(avatar->steps[1].kind, EventStepKind::KillCreature);
    EXPECT_EQ(avatar->steps[1].creatureEntry, 8443u);  // Avatar
    EXPECT_TRUE(avatar->steps[1].engage);

    // Event 10 — Weaver & Dreamscythe (required spine, Persistent), both engaged.
    DungeonEvent const* wd = DungeonEventRegistry::Find(109, 10);
    ASSERT_NE(wd, nullptr);
    EXPECT_TRUE(wd->persistent);
    EXPECT_TRUE(wd->required);
    ASSERT_EQ(wd->steps.size(), 2u);
    EXPECT_EQ(wd->steps[0].creatureEntry, 5720u);  // Weaver
    EXPECT_TRUE(wd->steps[0].engage);
    EXPECT_EQ(wd->steps[1].creatureEntry, 5721u);  // Dreamscythe
    EXPECT_TRUE(wd->steps[1].engage);

    // Event 11 — Atal'alarion (optional pit wing, Persistent), engaged.
    DungeonEvent const* atal = DungeonEventRegistry::Find(109, 11);
    ASSERT_NE(atal, nullptr);
    EXPECT_TRUE(atal->persistent);
    EXPECT_FALSE(atal->required);
    ASSERT_EQ(atal->steps.size(), 1u);
    EXPECT_EQ(atal->steps[0].kind, EventStepKind::KillCreature);
    EXPECT_EQ(atal->steps[0].creatureEntry, 8580u);
    EXPECT_TRUE(atal->steps[0].engage);
}

// Event 17 — central-circle PRE-CLEAR (before Jammal'an): a ClearRadius step, a
// position-based area sweep, on a Persistent + Optional objective. Cleared
// before Weaver/Dreamscythe un-phase and circle the floor.
TEST(DungeonEventRegistryTest, SunkenTempleCentralCirclePreClear)
{
    DungeonEvent const* circle = DungeonEventRegistry::Find(109, 17);
    ASSERT_NE(circle, nullptr);
    EXPECT_EQ(circle->activation, EventActivation::Anchored);
    EXPECT_TRUE(circle->persistent);
    EXPECT_FALSE(circle->required);
    ASSERT_EQ(circle->steps.size(), 1u);
    EXPECT_EQ(circle->steps[0].kind, EventStepKind::ClearRadius);
    EXPECT_TRUE(circle->steps[0].engage);          // driving action seeks & fights
    EXPECT_GT(circle->steps[0].radius, 0.0f);
    EXPECT_GT(circle->steps[0].zBand, 0.0f);       // floor band set
}

// The ClearRadius builder packs the centre, radius and floor z-band, and marks
// the step engage-driven (the objective action walks the tank in to fight).
TEST(DungeonEventBuilderTest, ClearRadiusStep)
{
    DungeonEvent e = EventBuilder(1, 1, "e")
                         .ClearRadius(10.0f, 20.0f, 30.0f, 55.0f, /*zBand*/ 18.0f)
                         .Build();
    ASSERT_EQ(e.steps.size(), 1u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::ClearRadius);
    EXPECT_FLOAT_EQ(e.steps[0].x, 10.0f);
    EXPECT_FLOAT_EQ(e.steps[0].y, 20.0f);
    EXPECT_FLOAT_EQ(e.steps[0].z, 30.0f);
    EXPECT_FLOAT_EQ(e.steps[0].radius, 55.0f);
    EXPECT_FLOAT_EQ(e.steps[0].zBand, 18.0f);
    EXPECT_TRUE(e.steps[0].engage);
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

    // 2. GARRISON the ramp head (MoveTo with a monotonic instance-phase gate):
    //    hold there — re-moving if combat displaced the tank — until DATA_PYRAMID
    //    (0) reaches WAVE_3 (7).
    EXPECT_EQ(e->steps[2].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e->steps[2].instanceDataId, 0);   // DATA_PYRAMID
    EXPECT_EQ(e->steps[2].instanceDataMin, 7u);  // PYRAMID_WAVE_3

    // 3. descend and kill the two temple bosses (engage-driven).
    EXPECT_EQ(e->steps[3].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[3].creatureEntry, 7796u);  // Nekrum
    EXPECT_TRUE(e->steps[3].engage);
    EXPECT_EQ(e->steps[4].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[4].creatureEntry, 7275u);  // Sezz'ziz
    EXPECT_TRUE(e->steps[4].engage);

    // 4. goblin FIRST (opens the door), then a short dwell before provoking Bly.
    //    Both gossips skip if the NPC is dead so a lost helper can't deadlock, and
    //    wait for the crew to finish walking down before talking.
    EXPECT_EQ(e->steps[5].kind, EventStepKind::Gossip);
    EXPECT_EQ(e->steps[5].creatureEntry, 7607u);  // Weegli
    EXPECT_TRUE(e->steps[5].skipIfMissing);
    EXPECT_TRUE(e->steps[5].waitForStill);
    EXPECT_EQ(e->steps[6].kind, EventStepKind::Wait);
    EXPECT_GT(e->steps[6].durationMs, 0u);

    // 5. human starts the fight; killing Bly ends the event.
    EXPECT_EQ(e->steps[7].kind, EventStepKind::Gossip);
    EXPECT_EQ(e->steps[7].creatureEntry, 7604u);  // Bly
    EXPECT_TRUE(e->steps[7].skipIfMissing);
    EXPECT_TRUE(e->steps[7].waitForStill);
    EXPECT_EQ(e->steps[8].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[8].creatureEntry, 7604u);
    EXPECT_TRUE(e->steps[8].engage);
}

// ZulFarrak Sacred Pool (Gahz'rilla) event (map 209, id 2): a PERSISTENT anchored
// step list ordered LAST (anchor at encounterIndex 8). Ring the gong (UseGO 141832)
// to summon Gahz'rilla, WAIT for it to emerge, then engage and kill it. The
// WaitForSpawn between the ring and the kill is essential — without it the kill
// step would read "no live boss" before the summon and false-complete.
TEST(DungeonEventRegistryTest, ZulFarrakGahzrillaEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(209, 2);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_TRUE(e->persistent);
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 3u);

    // 1. ring the gong -> summons Gahz'rilla (go->Use cheats the lock, no mallet).
    EXPECT_EQ(e->steps[0].kind, EventStepKind::UseGameObject);
    EXPECT_EQ(e->steps[0].goEntry, 141832u);

    // 2. hold until the summoned boss has materialised.
    EXPECT_EQ(e->steps[1].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(e->steps[1].creatureEntry, 7273u);
    EXPECT_TRUE(e->steps[1].wantAlive);

    // 3. engage and kill it (the pool boss does not auto-aggro).
    EXPECT_EQ(e->steps[2].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[2].creatureEntry, 7273u);
    EXPECT_TRUE(e->steps[2].engage);
}

// Blackrock Depths Ring of Law (map 230): a PERSISTENT anchored event that walks
// the tank onto the centre trigger, ensures the encounter started (Custom
// fallback), then holds dead-centre until TYPE_RING_OF_LAW (1) reaches DONE (3)
// while the random waves + boss are fought reactively. NOT a ClearRadius/count
// gate (those would false-complete in the empty-floor windows).
TEST(DungeonEventRegistryTest, BlackrockRingOfLawEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(230, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_EQ(e->orderIndex, 3u);  // between Grebmar (2) and Loregrain (4)
    EXPECT_TRUE(e->persistent);
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 3u);

    // 1. settle on the arena centre (area trigger 1526 spot).
    EXPECT_EQ(e->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(e->steps[0].x, 596.432f);
    EXPECT_EQ(e->steps[0].instanceDataId, -1);  // plain MoveTo, no gate

    // 2. ensure the encounter started (Custom -> EnsureRingStarted hook id 1).
    EXPECT_EQ(e->steps[1].kind, EventStepKind::Custom);
    EXPECT_EQ(e->steps[1].hookId, 1u);

    // 3. garrison the centre until TYPE_RING_OF_LAW (1) reaches DONE (3); long
    //    timeout for the boss fight.
    EXPECT_EQ(e->steps[2].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e->steps[2].instanceDataId, 1);    // TYPE_RING_OF_LAW
    EXPECT_EQ(e->steps[2].instanceDataMin, 3u);  // EncounterState::DONE
    EXPECT_EQ(e->steps[2].timeoutMs, 600000u);
}

// Deadmines Defias Cannon: walk to the cannon, fire it (Custom hook 2 casts the
// gunpowder spell at GO 16398), then hold until the Iron Clad Door (16397) opens.
TEST(DungeonEventRegistryTest, DeadminesCannonEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(36, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_EQ(e->orderIndex, 3u);  // between Gilnid (2) and Mr. Smite (3)
    EXPECT_TRUE(e->persistent);
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 3u);

    // 1. step onto the cannon.
    EXPECT_EQ(e->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(e->steps[0].x, -107.56f);

    // 2. fire it (Custom -> FireDefiasCannon hook id 2).
    EXPECT_EQ(e->steps[1].kind, EventStepKind::Custom);
    EXPECT_EQ(e->steps[1].hookId, 2u);

    // 3. hold until the Iron Clad Door opens (GO_STATE_ACTIVE_ALTERNATIVE).
    EXPECT_EQ(e->steps[2].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(e->steps[2].goEntry, 16397u);
    EXPECT_EQ(e->steps[2].wantState, 2u);
    EXPECT_EQ(e->steps[2].timeoutMs, 30000u);
}

// Wailing Caverns drop to Lord Serpentis: settle on the lip, then jump the
// off-mesh gap onto Serpentis's shelf. Persistent (the drop is one-way; a rewind
// would walk back to the now-unreachable lip).
TEST(DungeonEventRegistryTest, WailingCavernsSerpentisDropEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(43, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_EQ(e->orderIndex, 5u);  // shared with Serpentis (bit 5)
    EXPECT_TRUE(e->persistent);
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 2u);

    // 1. settle on the jump lip.
    EXPECT_EQ(e->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(e->steps[0].x, -290.65567f);
    EXPECT_FLOAT_EQ(e->steps[0].radius, 3.0f);

    // 2. leap onto the landing shelf.
    EXPECT_EQ(e->steps[1].kind, EventStepKind::Jump);
    EXPECT_FLOAT_EQ(e->steps[1].x, -285.45773f);
    EXPECT_FLOAT_EQ(e->steps[1].y, 4.021016f);
    EXPECT_FLOAT_EQ(e->steps[1].z, -63.919395f);
    EXPECT_FLOAT_EQ(e->steps[1].radius, 5.0f);
}

// Stratholme (329) dead-side "Baron run": the persistent Slaughterhouse chain
// (eventId 4), anchored at Ramstein's DBC bit 11 (after the ziggurats + Barthilas,
// before Baron 12). Abominations+Ramstein (one ClearRadius) -> wait+clear undead
// wave (ClearRadius) -> wait+kill guard wave (KillCreatureEngage, since the
// guards post far off and a bot-centred ClearRadius can't see them) -> gate on
// the Baron door opening. Slaughter progress isn't exposed via GetData, so the
// phase barriers are the monotonic doors (combat-gap proof), not transient
// creature checks.
TEST(DungeonEventRegistryTest, StratholmeSlaughterhouseEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(329, 4);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_EQ(e->orderIndex, 11u);  // Ramstein's bit, between Barthilas (10) and Baron (12)
    EXPECT_TRUE(e->persistent);
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 6u);

    // 1. clear the hall: abominations + the synchronously-summoned Ramstein.
    EXPECT_EQ(e->steps[0].kind, EventStepKind::ClearRadius);
    EXPECT_TRUE(e->steps[0].engage);
    EXPECT_FLOAT_EQ(e->steps[0].x, 4032.0f);
    EXPECT_FLOAT_EQ(e->steps[0].y, -3415.0f);

    // 2. wave 1: wait for the mindless undead, then ClearRadius them.
    EXPECT_EQ(e->steps[1].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(e->steps[1].creatureEntry, 11030u);
    EXPECT_TRUE(e->steps[1].wantAlive);
    EXPECT_EQ(e->steps[2].kind, EventStepKind::ClearRadius);

    // 3. wave 2: wait for the black guards, then actively seek+kill them
    //    (KillCreatureEngage) — they post far off, so a bot-centred ClearRadius
    //    can't see them.
    EXPECT_EQ(e->steps[3].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(e->steps[3].creatureEntry, 10394u);
    EXPECT_EQ(e->steps[4].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[4].creatureEntry, 10394u);

    // 4. monotonic completion gate: the Baron door (175796) opens when the guards
    //    die. GO_STATE_ACTIVE (0) = open.
    EXPECT_EQ(e->steps[5].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(e->steps[5].goEntry, 175796u);
    EXPECT_EQ(e->steps[5].wantState, 0u);
    EXPECT_GT(e->steps[5].radius, 100.0f);  // reaches the door from across the hall
}

// Stratholme (329) live side: Grand Crusader Dathrohan -> Balnazzar (eventId 5),
// anchored at the Dathrohan objective (bit 6, after Galford). Balnazzar (10813)
// has no spawn — he is Dathrohan (10812) after an UpdateEntry at 40% HP — so two
// KillCreatureEngage steps: pull 10812 (Done when he transforms away), then hold
// on 10813 until he's dead. Non-persistent: both steps are idempotent kill-gates.
TEST(DungeonEventRegistryTest, StratholmeDathrohanBalnazzarEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(329, 5);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_EQ(e->orderIndex, 6u);  // Balnazzar's DBC bit, right after Galford (5)
    EXPECT_FALSE(e->persistent);
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 2u);

    // 1. seek + engage Dathrohan; Done once he UpdateEntry's away (no live 10812).
    EXPECT_EQ(e->steps[0].kind, EventStepKind::KillCreature);
    EXPECT_TRUE(e->steps[0].engage);
    EXPECT_EQ(e->steps[0].creatureEntry, 10812u);

    // 2. finish the transformed Balnazzar; Done when 10813 is dead.
    EXPECT_EQ(e->steps[1].kind, EventStepKind::KillCreature);
    EXPECT_TRUE(e->steps[1].engage);
    EXPECT_EQ(e->steps[1].creatureEntry, 10813u);
}

// The three ziggurat acolyte clears (eventIds 1/2/3) are conditional events
// (conditions 5/6/7) that fire when a ziggurat door is open but the chamber not
// yet cleared, each a single ClearRadius of its acolyte chamber.
TEST(DungeonEventConditional, StratholmeZigguratAcolyteEvents)
{
    std::vector<DungeonEvent const*> str = DungeonEventRegistry::Conditional(329);
    ASSERT_EQ(str.size(), 3u);  // the 3 ziggurats; the Slaughterhouse is anchored
    for (DungeonEvent const* e : str)
    {
        EXPECT_EQ(e->activation, EventActivation::Conditional);
        EXPECT_TRUE(e->required);
        ASSERT_EQ(e->steps.size(), 1u);
        EXPECT_EQ(e->steps[0].kind, EventStepKind::ClearRadius);
        EXPECT_TRUE(e->steps[0].engage);
    }

    // event id N maps to condition N+4 (1->5, 2->6, 3->7).
    for (uint32 id : {1u, 2u, 3u})
    {
        DungeonEvent const* e = DungeonEventRegistry::Find(329, id);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->conditionId, id + 4u);
        EXPECT_TRUE(EventConditionRegistry::Has(id + 4u));
    }

    // Each acolyte clear sorts in the panel just before the next anchor (so it
    // renders right after the boss whose door it follows, not dumped at the end):
    // zig1 -> before Nerub'enkan, zig2 -> before Maleki, zig3 -> before the
    // Slaughterhouse objective.
    EXPECT_EQ(DungeonEventRegistry::Find(329, 1)->panelGatesBossEntry, 10437u);
    EXPECT_EQ(DungeonEventRegistry::Find(329, 2)->panelGatesBossEntry, 10438u);
    EXPECT_EQ(DungeonEventRegistry::Find(329, 3)->panelGatesBossEntry,
              BossRosterRegistry::ObjectiveEntry(1));

    // These are NOT room-aggro pre-clears (ClearRadius, not KillCreature(0)).
    EXPECT_FALSE(DungeonEventRegistry::HasRoomAggroEvent(329));
}

// Uldaman (70): the Ironaya seal — a conditional event that clears the
// antechamber, uses the keystone, then waits for the Seal of Khaz'Mul to open.
TEST(DungeonEventConditional, UldamanIronayaSeal)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(70, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Conditional);
    EXPECT_EQ(e->conditionId, 8u);
    EXPECT_TRUE(EventConditionRegistry::Has(8u));
    EXPECT_TRUE(e->required);
    EXPECT_FALSE(e->repeatable);

    ASSERT_EQ(e->steps.size(), 4u);
    EXPECT_EQ(e->steps[0].kind, EventStepKind::ClearRadius);
    EXPECT_TRUE(e->steps[0].engage);
    EXPECT_EQ(e->steps[1].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e->steps[2].kind, EventStepKind::UseGameObject);
    EXPECT_EQ(e->steps[2].goEntry, 124371u);            // the Keystone
    EXPECT_EQ(e->steps[3].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(e->steps[3].goEntry, 124372u);            // the Seal of Khaz'Mul
    EXPECT_EQ(e->steps[3].wantState, 0u);               // GO_STATE_ACTIVE (open)

    // Uldaman has ONE conditional event (the Ironaya seal — its antechamber has
    // live Stonevault trash whose ClearRadius seek walks the tank in). The Altar
    // of the Keepers and Altar of Archaedas are ANCHORED events on roster
    // objectives instead (the halls are dormant immune statues with nothing to
    // seek, so a conditional gate can't navigate the tank in). NO room-aggro
    // pre-clear (the seal is a multi-step ClearRadius, not the lone
    // KillCreature(0) shape).
    EXPECT_EQ(DungeonEventRegistry::Conditional(70).size(), 1u);
    EXPECT_FALSE(DungeonEventRegistry::IsRoomAggroPreClear(*e));
    EXPECT_FALSE(DungeonEventRegistry::HasRoomAggroEvent(70));
}

// Uldaman (70): the Altar of the Keepers — an ANCHORED event on roster objective
// OBJ(1). Boss-nav delivers the tank into the hall; the event clears the live
// trash (Stewards / Earthen), centres on the altar, fires the SEND_EVENT to
// awaken the 4 stoned keepers, kills them, then waits for the temple door.
// Persistent so the multi-keeper fight can't rewind it.
TEST(DungeonEventAnchored, UldamanStoneKeepers)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(70, 2);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_FALSE(EventConditionRegistry::Has(9u));       // condition 9 retired
    EXPECT_TRUE(e->required);
    EXPECT_TRUE(e->persistent);

    ASSERT_EQ(e->steps.size(), 5u);
    EXPECT_EQ(e->steps[0].kind, EventStepKind::ClearRadius);  // clear live hall trash
    EXPECT_TRUE(e->steps[0].engage);
    EXPECT_EQ(e->steps[1].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e->steps[2].kind, EventStepKind::CastSpell);
    EXPECT_EQ(e->steps[2].spellId, 11568u);              // Altar of The Keepers SEND_EVENT
    EXPECT_EQ(e->steps[3].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[3].creatureEntry, 4857u);         // Stone Keeper
    EXPECT_FALSE(e->steps[3].engage);                    // plain gate (party auto-aggros)
    EXPECT_EQ(e->steps[4].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(e->steps[4].goEntry, 124367u);             // temple door
    EXPECT_EQ(e->steps[4].wantState, 0u);                // GO_STATE_ACTIVE (open)
}

// Uldaman (70): the Altar of Archaedas — an ANCHORED event on roster objective
// OBJ(2). Boss-nav delivers the tank onto the altar, this fires its SEND_EVENT to
// wake the stoned final boss, and the boss pull then kills him.
TEST(DungeonEventAnchored, UldamanArchaedasAltar)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(70, 3);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Anchored);
    EXPECT_FALSE(EventConditionRegistry::Has(10u));      // condition 10 retired
    EXPECT_TRUE(e->required);

    ASSERT_EQ(e->steps.size(), 2u);
    EXPECT_EQ(e->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e->steps[1].kind, EventStepKind::CastSpell);
    EXPECT_EQ(e->steps[1].spellId, 10340u);             // Altar of Archaedas SEND_EVENT
}

// Garrison MoveTo (MoveToHoldUntilSpawn): a MoveTo step carrying a spawn-gate
// creature, so the executor holds at the point until that creature is up.
TEST(DungeonEventBuilderTest, MoveToHoldUntilSpawn)
{
    DungeonEvent e = EventBuilder(1, 1, "e")
                         .MoveTo(1.0f, 2.0f, 3.0f, 5.0f)
                         .MoveToHoldUntilSpawn(4.0f, 5.0f, 6.0f, 8.0f, /*until*/ 7275)
                         .Build();
    ASSERT_EQ(e.steps.size(), 2u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e.steps[0].creatureEntry, 0u);  // plain MoveTo, no gate
    EXPECT_EQ(e.steps[1].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e.steps[1].creatureEntry, 7275u);  // garrison gate
    EXPECT_TRUE(e.steps[1].wantAlive);
    EXPECT_EQ(e.steps[1].instanceDataId, -1);  // no instance gate on the spawn variant
}

// Garrison MoveTo, instance-data variant: a MoveTo carrying a monotonic phase
// gate (GetData(id) >= min), preferred for content the party kills mid-combat.
TEST(DungeonEventBuilderTest, MoveToHoldUntilInstanceData)
{
    DungeonEvent e = EventBuilder(1, 1, "e")
                         .MoveToHoldUntilInstanceData(1.0f, 2.0f, 3.0f, 10.0f,
                                                      /*dataId*/ 0, /*min*/ 7)
                         .Build();
    ASSERT_EQ(e.steps.size(), 1u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);
    EXPECT_EQ(e.steps[0].instanceDataId, 0);
    EXPECT_EQ(e.steps[0].instanceDataMin, 7u);
    EXPECT_EQ(e.steps[0].creatureEntry, 0u);  // instance gate, not a creature gate
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

// SkipIfTargetMissing / WaitTargetStill flag the last-added step's gossip bits.
TEST(DungeonEventBuilderTest, SkipIfTargetMissing)
{
    DungeonEvent e = EventBuilder(1, 1, "e")
                         .Gossip(100, 0)
                         .Gossip(200, 0).SkipIfTargetMissing().WaitTargetStill()
                         .Build();
    ASSERT_EQ(e.steps.size(), 2u);
    EXPECT_FALSE(e.steps[0].skipIfMissing);
    EXPECT_FALSE(e.steps[0].waitForStill);
    EXPECT_TRUE(e.steps[1].skipIfMissing);
    EXPECT_TRUE(e.steps[1].waitForStill);
}

// --- Milestone 2: conditional activation ----------------------------------

// Conditional() returns only EventActivation::Conditional events for the map.
// Sunken Temple (109) is anchored-only (forcefield ring anchors + statues etc.);
// Shadowfang Keep (33) has the conditional courtyard-door event; ZulFarrak (209)
// is anchored only.
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

// Scholomance (289) re-uses the same room-aggro pre-clear shape for the merged
// Marduk & Vectus boss: Conditional(3) + a lone KillCreature(0) room-trash step.
TEST(DungeonEventRoomAggro, ScholomanceMardukVectusEventShape)
{
    DungeonEvent const* e = DungeonEventRegistry::Find(289, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->activation, EventActivation::Conditional);
    EXPECT_EQ(e->conditionId, 3u);
    EXPECT_TRUE(e->required);
    ASSERT_EQ(e->steps.size(), 1u);
    EXPECT_EQ(e->steps[0].kind, EventStepKind::KillCreature);
    EXPECT_EQ(e->steps[0].creatureEntry, 0u);  // room-trash mode

    EXPECT_TRUE(DungeonEventRegistry::IsRoomAggroPreClear(*e));
    EXPECT_TRUE(DungeonEventRegistry::HasRoomAggroEvent(289));
}

// IsRoomAggroPreClear distinguishes the room-trash gate from the step-driven
// SFK gossip events and from anchored objectives — only the lone-KillCreature(0)
// Conditional shape qualifies, so HasRoomAggroEvent flags only those maps.
TEST(DungeonEventRoomAggro, PredicateAndHasRoomAggroEvent)
{
    DungeonEvent const* cath = DungeonEventRegistry::Find(189, 1);
    DungeonEvent const* sfk = DungeonEventRegistry::Find(33, 1);   // gossip event
    DungeonEvent const* st = DungeonEventRegistry::Find(109, 1);   // forcefield anchor
    ASSERT_NE(cath, nullptr);
    ASSERT_NE(sfk, nullptr);
    ASSERT_NE(st, nullptr);

    EXPECT_TRUE(DungeonEventRegistry::IsRoomAggroPreClear(*cath));
    EXPECT_FALSE(DungeonEventRegistry::IsRoomAggroPreClear(*sfk));
    // ST's forcefield is an Anchored ring-anchor event, not the lone
    // Conditional KillCreature(0) room-trash shape.
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

// --- Dire Maul West (map 429) --------------------------------------------

// Immol'thar's five Crystal Generator pylons (events 4-8) are Anchored UseGO +
// Wait objectives — Persistent (a combat gap at a guarded pylon must not rewind
// and re-click the spent BUTTON) and Optional (a misfire degrades to standing at
// the still-shielded boss). Each clicks one generator GO; the instance flips the
// pylon bit and, at all five, makes Immol'thar attackable.
TEST(DungeonEventAnchored, DireMaulWestPylonEventShape)
{
    struct Pylon { uint32 eventId; uint32 goEntry; };
    for (Pylon const& p : { Pylon{4, 177259}, Pylon{5, 177257}, Pylon{6, 177258},
                            Pylon{7, 179504}, Pylon{8, 179505} })
    {
        DungeonEvent const* e = DungeonEventRegistry::Find(429, p.eventId);
        ASSERT_NE(e, nullptr) << "missing pylon event " << p.eventId;
        EXPECT_EQ(e->activation, EventActivation::Anchored);
        EXPECT_TRUE(e->persistent);
        EXPECT_FALSE(e->required);  // Optional
        // Uldaman keeper pattern: clear the guards, close to the crystal, click,
        // wait. ClearRadius first (kill guards/treants), then MoveTo (reach the
        // dais), then UseGO (click), then Wait (activation delay).
        ASSERT_EQ(e->steps.size(), 4u);
        EXPECT_EQ(e->steps[0].kind, EventStepKind::ClearRadius);
        EXPECT_GT(e->steps[0].radius, 15.0f);      // covers guards + blink hop
        EXPECT_GT(e->steps[0].timeoutMs, 30000u);  // generous for a caster pack
        EXPECT_EQ(e->steps[1].kind, EventStepKind::MoveTo);
        EXPECT_EQ(e->steps[2].kind, EventStepKind::UseGameObject);
        EXPECT_EQ(e->steps[2].goEntry, p.goEntry);
        EXPECT_EQ(e->steps[3].kind, EventStepKind::Wait);
        EXPECT_GT(e->steps[3].durationMs, 0u);
    }
}

// There is no dedicated standalone Warpwood entrance-clear event (429/11) — that
// unreachable dais anchor deadlocked. The entrance/guard clearing lives inside
// the crystal events themselves (ClearRadius step), paired with a large
// arriveRadius in the roster so it doesn't compete with the travel-to-crystal.
TEST(DungeonEventAnchored, DireMaulWestNoStandaloneEntranceEvent)
{
    EXPECT_EQ(DungeonEventRegistry::Find(429, 11), nullptr);
}

// The two Crescent Key doors (events 9/10, conditions 14/15) are on-path
// Conditional door events — the same UseGO + WaitForGameObjectState shape as the
// Gordok doors, so they preempt the door-blocked stall. GameObject::Use on a DOOR
// ignores the lock, so no Crescent Key is needed.
TEST(DungeonEventConditional, DireMaulWestCrescentDoorEventShape)
{
    struct Door { uint32 eventId; uint32 conditionId; uint32 goEntry; };
    for (Door const& d : { Door{9, 14, 177221}, Door{10, 15, 179550} })
    {
        DungeonEvent const* e = DungeonEventRegistry::Find(429, d.eventId);
        ASSERT_NE(e, nullptr) << "missing crescent door event " << d.eventId;
        EXPECT_EQ(e->activation, EventActivation::Conditional);
        EXPECT_EQ(e->conditionId, d.conditionId);
        EXPECT_FALSE(e->required);  // Optional
        ASSERT_EQ(e->steps.size(), 2u);
        EXPECT_EQ(e->steps[0].kind, EventStepKind::UseGameObject);
        EXPECT_EQ(e->steps[0].goEntry, d.goEntry);
        EXPECT_EQ(e->steps[1].kind, EventStepKind::WaitForGameObjectState);
        EXPECT_EQ(e->steps[1].goEntry, d.goEntry);
        EXPECT_EQ(e->steps[1].wantState, 0u);  // GO_STATE_ACTIVE (open)

        EXPECT_TRUE(EventConditionRegistry::Has(d.conditionId));
    }
}
