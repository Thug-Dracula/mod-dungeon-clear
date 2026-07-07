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
