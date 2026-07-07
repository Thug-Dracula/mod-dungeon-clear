/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"

// Registry cross-reference + persistence lint over the authored event data
// (events-system review F1/F2/F3, plus the F9 anchored-event wiring check). These
// registries reference each other by bare integers, and every dangling reference
// fails SILENTLY at runtime (an objective latches Done instantly, a required event
// never fires, an objective vanishes from a wing). These tests turn each of those
// silent-failure classes into a red test at build time.

namespace
{
    // A step kind whose rewind-on-gap is dangerous: teleport/drop/jump are one-way,
    // escort/engage/clear span combat gaps, an instance-data MoveTo garrisons a
    // gate. A multi-step anchored event containing one of these MUST be Persistent
    // or a mid-fight combat gap rewinds it (the module's most-repeated bug class —
    // see the dc-multihop-teleport-persistent memory). A KillCreatureEngage is a
    // KillCreature step with engage set.
    bool IsRewindHazardStep(EventStep const& s)
    {
        switch (s.kind)
        {
            case EventStepKind::TeleportParty:
            case EventStepKind::DropInHole:
            case EventStepKind::Jump:
            case EventStepKind::EscortCreature:
            case EventStepKind::ClearRadius:
                return true;
            case EventStepKind::KillCreature:
                return s.engage;  // KillCreatureEngage seeks + pulls across gaps
            case EventStepKind::MoveTo:
                return s.instanceDataId >= 0;  // instance-data garrison gate
            default:
                return false;
        }
    }

    bool HasRewindHazard(DungeonEvent const& ev)
    {
        for (EventStep const& s : ev.steps)
            if (IsRewindHazardStep(s))
                return true;
        return false;
    }

    // Intentionally non-persistent multi-step anchored events (each carries an
    // in-file justification). Whitelisted so the F1 lint stays green while still
    // catching the next author who forgets .Persistent(). Keyed by {mapId, eventId}.
    //   - Stratholme (329) event 5 "Dathrohan -> Balnazzar": two idempotent
    //     KillCreatureEngage kill-gates, one continuous fight at a fixed spot, no
    //     WaitForSpawn to false-complete across a gap — a restart-from-0 re-evaluates
    //     correctly (StratholmeEvents.cpp:29-35).
    bool IsNonPersistentWhitelisted(uint32 mapId, uint32 eventId)
    {
        return mapId == 329 && eventId == 5;
    }

    // An "arrival" step forces the tank to a specific spot before it can report
    // Done: MoveTo/Jump HopTo until within radius, UseGameObject/Gossip approach
    // their target, and the escort/drop/teleport primitives all pin the leader to
    // a checkpoint. A conditional event that contains ONE of these can never
    // complete while the tank is far from the anchor. Everything else (ClearRadius,
    // KillCreature, WaitForSpawn/GOState, Wait, CastSpell, UseItem, Custom) can
    // report Done from wherever the tank stands the instant the condition turns
    // true — so an event built ONLY from those latches "done" from afar.
    bool HasArrivalStep(DungeonEvent const& ev)
    {
        for (EventStep const& s : ev.steps)
            switch (s.kind)
            {
                case EventStepKind::MoveTo:
                case EventStepKind::Jump:
                case EventStepKind::UseGameObject:
                case EventStepKind::Gossip:
                case EventStepKind::EscortCreature:
                case EventStepKind::DropInHole:
                case EventStepKind::TeleportParty:
                    return true;
                default:
                    break;
            }
        return false;
    }

    // Conditional events that legitimately complete WITHOUT any arrival step —
    // their single/all-gate step list is safe ONLY because their activation
    // predicate can't read true while the tank is far from the anchor. That
    // near-only guarantee lives in the opaque condition function (a test can't
    // inspect it), so each such event is vetted by hand and listed here with the
    // mechanism that makes it near-only. Keyed by {mapId, eventId}. See the
    // ConditionalEventsWithoutArrivalStepAreProximityVetted tripwire.
    bool IsNearGatedConditionalWhitelisted(uint32 mapId, uint32 eventId)
    {
        struct Row { uint32 mapId; uint32 eventId; };
        static constexpr Row kRows[] = {
            // Stratholme Timmy pre-clear: StrTimmyGated returns false until the
            // tank is within 60yd of Timmy's spawn (the #5 fix — a creature-
            // presence condition read true from map load and false-latched at the
            // instance entrance without this gate).
            {329, 6},
            // Stratholme ziggurat acolyte clears: gated on monotonic instance data
            // (GetData(TYPE_ZIGGURATx) == 1), which flips only when the ziggurat
            // boss dies right at the chamber — never true from afar.
            {329, 1},
            {329, 2},
            {329, 3},
        };
        for (Row const& r : kRows)
            if (r.mapId == mapId && r.eventId == eventId)
                return true;
        return false;
    }
}

// --- sanity ---------------------------------------------------------------

TEST(DungeonEventIntegrityTest, EventTableIsNonEmpty)
{
    EXPECT_FALSE(DungeonEventRegistry::AllEvents().empty());
}

// --- F2: dangling cross-references ----------------------------------------

TEST(DungeonEventIntegrityTest, ConditionalEventsHaveBoundCondition)
{
    // A Conditional event with no bound predicate never fires, so a Required one is
    // a silent wall (the run stalls at a shut door, never naming the event). With
    // the id space replaced by a function pointer (.Conditional(&Predicate)), a
    // wrong name is now a compile error; this lint still catches the one remaining
    // authoring slip — a Conditional() call that was never given a predicate.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Conditional)
            continue;
        EXPECT_TRUE(static_cast<bool>(ev.condition))
            << "conditional event map " << ev.mapId << " id " << ev.id
            << " (" << ev.name << ") has no bound condition — it can never fire";
    }
}

// Tripwire for the Stratholme #5 bug class: a Conditional event whose steps can
// ALL report Done from wherever the tank stands (no arrival step) latches
// "complete" the instant its condition turns true — even with the tank at the
// far end of the map. The executor evaluates its first ClearRadius/KillCreature
// gate from that far position, finds nothing engage-reachable, returns Done, and
// the event is marked done with nothing accomplished (Timmy's room "already
// cleared" at the instance entrance). Such an event is sound ONLY if its
// condition can't read true while the tank is far — a property that lives in the
// opaque predicate a test can't inspect. So force a conscious opt-in: any such
// event must be either a room-aggro pre-clear (condition proximity-gated inside
// RoomTrashRemaining) or hand-vetted on IsNearGatedConditionalWhitelisted. A NEW
// no-arrival conditional event trips this until its author does one of those —
// or, better, gives it an arrival step / proximity-gated condition.
TEST(DungeonEventIntegrityTest, ConditionalEventsWithoutArrivalStepAreProximityVetted)
{
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Conditional || ev.steps.empty())
            continue;
        if (HasArrivalStep(ev))
            continue;  // an arrival step forces the tank on-site before completion
        if (DungeonEventRegistry::IsRoomAggroPreClear(ev))
            continue;  // condition is proximity-gated inside RoomTrashRemaining

        EXPECT_TRUE(IsNearGatedConditionalWhitelisted(ev.mapId, ev.id))
            << "conditional event map " << ev.mapId << " id " << ev.id << " ("
            << ev.name << ") has no arrival step (MoveTo/UseGO/Gossip/...), so every "
            << "step can report Done from afar and it false-latches 'complete' the "
            << "instant its condition turns true with the tank far from the anchor "
            << "(the Stratholme #5 'Timmy room already cleared at the entrance' bug). "
            << "Fix: give it a proximity-gated condition (like StrTimmyGated's 60yd "
            << "check) or an arrival step; then, if verified near-only, add it to "
            << "IsNearGatedConditionalWhitelisted with the gating mechanism noted.";
    }
}

TEST(DungeonEventIntegrityTest, CustomStepsHaveRegisteredHook)
{
    // A Custom step with an unregistered hookId now Blocks (was: silently Done) —
    // catch the typo at author time before it stalls a live run.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        for (EventStep const& s : ev.steps)
        {
            if (s.kind != EventStepKind::Custom)
                continue;
            EXPECT_NE(s.hookId, 0u)
                << "Custom step in event map " << ev.mapId << " id " << ev.id
                << " (" << ev.name << ") has hookId 0";
            EXPECT_TRUE(ObjectiveHookRegistry::Has(s.hookId))
                << "Custom step in event map " << ev.mapId << " id " << ev.id
                << " (" << ev.name << ") references unregistered hookId " << s.hookId;
        }
    }
}

TEST(DungeonEventIntegrityTest, RosterObjectiveEventIdsResolveToAnchoredEvents)
{
    // Every objective's eventId must resolve; a typo falls into the legacy hook
    // path (onArriveHook 0 -> Done) and the objective latches instantly on arrival,
    // silently skipping the gate it guards.
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        for (DungeonBossInfo const& e : patch.add)
        {
            if (e.kind != DungeonAnchorKind::Objective || e.eventId == 0)
                continue;
            DungeonEvent const* ev = DungeonEventRegistry::Find(patch.mapId, e.eventId);
            ASSERT_NE(ev, nullptr)
                << "roster objective entry " << e.entry << " on map " << patch.mapId
                << " (" << e.name << ") references non-existent eventId " << e.eventId;
            EXPECT_EQ(ev->activation, EventActivation::Anchored)
                << "objective-referenced event map " << patch.mapId << " id " << e.eventId
                << " must be Anchored, not Conditional";
        }
    }
}

TEST(DungeonEventIntegrityTest, RosterObjectiveHooksAreRegistered)
{
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        for (DungeonBossInfo const& e : patch.add)
        {
            if (e.kind != DungeonAnchorKind::Objective || e.onArriveHook == 0)
                continue;
            EXPECT_TRUE(ObjectiveHookRegistry::Has(e.onArriveHook))
                << "roster objective entry " << e.entry << " on map " << patch.mapId
                << " (" << e.name << ") references unregistered onArriveHook " << e.onArriveHook;
        }
    }
}

// --- F9: every anchored event is wired by exactly one objective -----------

TEST(DungeonEventIntegrityTest, AnchoredEventsAreWiredByExactlyOneObjective)
{
    // An anchored event enters the clear only through an objective anchor's eventId.
    // An anchored event no objective references is dead data (authored, never fired).
    // NOTE: DungeonEvent::orderIndex is doc-only and has drifted from the roster's
    // real order key in a few files (DireMaul pylons reuse the eventId, Uldaman uses
    // 7/8 vs roster 8/9) — so we assert the WIRING exists, not orderIndex equality.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Anchored)
            continue;
        int refs = 0;
        for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
        {
            if (patch.mapId != ev.mapId)
                continue;
            for (DungeonBossInfo const& e : patch.add)
                if (e.kind == DungeonAnchorKind::Objective && e.eventId == ev.id)
                    ++refs;
        }
        EXPECT_EQ(refs, 1)
            << "anchored event map " << ev.mapId << " id " << ev.id << " (" << ev.name
            << ") is wired by " << refs << " objectives (expected exactly 1)";
    }
}

// --- F1: persistence lint -------------------------------------------------

TEST(DungeonEventIntegrityTest, MultiStepRewindHazardEventsArePersistent)
{
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Anchored)
            continue;
        if (ev.steps.size() <= 1 || !HasRewindHazard(ev))
            continue;
        if (IsNonPersistentWhitelisted(ev.mapId, ev.id))
            continue;
        EXPECT_TRUE(ev.persistent)
            << "anchored event map " << ev.mapId << " id " << ev.id << " (" << ev.name
            << ") has >1 step and a rewind-hazard step but is not .Persistent() — a "
               "combat gap will rewind it to step 0. Add .Persistent() or whitelist it.";
    }
}

// --- F3: wing/roster sync -------------------------------------------------

TEST(DungeonEventIntegrityTest, IsolatedWingObjectivesAppearInExactlyOneWing)
{
    // On a physically-isolated split map the boss list is filtered to the bot's
    // wing; a synthetic objective NOT listed in a wing is silently dropped and
    // never cleared. Assert every added objective on such a map is in exactly one
    // wing.
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(patch.mapId);
        if (!layout || !layout->isolated)
            continue;  // single-wing, or label-only (Maraudon) — no filter applied
        for (DungeonBossInfo const& e : patch.add)
        {
            if (e.kind != DungeonAnchorKind::Objective)
                continue;
            int inWings = 0;
            for (DungeonWing const& w : layout->wings)
                for (uint32 entry : w.bossEntries)
                    if (entry == e.entry)
                        ++inWings;
            EXPECT_EQ(inWings, 1)
                << "objective entry " << e.entry << " (" << e.name << ") on isolated "
                << "wing-split map " << patch.mapId << " is listed in " << inWings
                << " wings (expected exactly 1) — it will be silently dropped from the clear";
        }
    }
}
