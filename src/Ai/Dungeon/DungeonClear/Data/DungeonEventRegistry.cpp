/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonEventRegistry.h"

#include <utility>

// --- EventBuilder ---------------------------------------------------------

EventBuilder::EventBuilder(uint32 mapId, uint32 id, std::string name)
{
    _ev.mapId = mapId;
    _ev.id = id;
    _ev.name = std::move(name);
}

EventStep& EventBuilder::Add(EventStepKind kind)
{
    _ev.steps.emplace_back();
    EventStep& s = _ev.steps.back();
    s.kind = kind;
    return s;
}

EventBuilder& EventBuilder::Anchored(uint32 orderIndex)
{
    _ev.activation = EventActivation::Anchored;
    _ev.orderIndex = orderIndex;
    return *this;
}

EventBuilder& EventBuilder::Conditional(uint32 conditionId)
{
    _ev.activation = EventActivation::Conditional;
    _ev.conditionId = conditionId;
    return *this;
}

EventBuilder& EventBuilder::Optional()
{
    _ev.required = false;
    return *this;
}

EventBuilder& EventBuilder::MoveTo(float x, float y, float z, float radius)
{
    EventStep& s = Add(EventStepKind::MoveTo);
    s.x = x;
    s.y = y;
    s.z = z;
    s.radius = radius;
    return *this;
}

EventBuilder& EventBuilder::UseGO(uint32 goEntry, float searchRadius,
                                  float x, float y, float z)
{
    EventStep& s = Add(EventStepKind::UseGameObject);
    s.goEntry = goEntry;
    s.radius = searchRadius;
    s.x = x;
    s.y = y;
    s.z = z;
    return *this;
}

EventBuilder& EventBuilder::Gossip(uint32 creatureEntry, int32 option, float searchRadius)
{
    EventStep& s = Add(EventStepKind::Gossip);
    s.creatureEntry = creatureEntry;
    s.gossipOption = option;
    s.radius = searchRadius;
    return *this;
}

EventBuilder& EventBuilder::WaitForSpawn(uint32 creatureEntry, bool wantAlive, uint32 timeoutMs)
{
    EventStep& s = Add(EventStepKind::WaitForSpawn);
    s.creatureEntry = creatureEntry;
    s.wantAlive = wantAlive;
    s.timeoutMs = timeoutMs;
    return *this;
}

EventBuilder& EventBuilder::WaitForGOState(uint32 goEntry, uint32 wantState,
                                           uint32 timeoutMs, float searchRadius)
{
    EventStep& s = Add(EventStepKind::WaitForGameObjectState);
    s.goEntry = goEntry;
    s.wantState = wantState;
    s.timeoutMs = timeoutMs;
    s.radius = searchRadius;
    return *this;
}

EventBuilder& EventBuilder::KillCreature(uint32 creatureEntry, uint32 count, float searchRadius)
{
    EventStep& s = Add(EventStepKind::KillCreature);
    s.creatureEntry = creatureEntry;
    s.count = count;
    s.radius = searchRadius;
    return *this;
}

EventBuilder& EventBuilder::Wait(uint32 durationMs)
{
    EventStep& s = Add(EventStepKind::Wait);
    s.durationMs = durationMs;
    return *this;
}

EventBuilder& EventBuilder::Custom(uint32 hookId)
{
    EventStep& s = Add(EventStepKind::Custom);
    s.hookId = hookId;
    return *this;
}

// --- The event table ------------------------------------------------------
// One block per dungeon. Event ids are per-map and are referenced from the
// matching travel-objective anchor's DungeonBossInfo::eventId (BossRosterRegistry).

namespace
{
    std::vector<DungeonEvent> const& EventTable()
    {
        static std::vector<DungeonEvent> const kEvents = []
        {
            std::vector<DungeonEvent> t;

            // --- Sunken Temple (map 109), event 1 --------------------------
            // The Altar of Hakkar objective: the tank leads the party to the
            // central altar (the anchor handles travel), then this event holds
            // there waiting for the Avatar of Hakkar (8443) to manifest from the
            // Atal'ai sacrifice. Optional: whether the summon triggers off the
            // trash the bots kill is unverified, so on timeout the event SKIPS
            // (the clear advances) rather than stalling forever — matching the
            // roster note that you may have to `dc skip` it. 45s tolerates a slow
            // approach + the scripted sequence.
            t.push_back(EventBuilder(109, 1, "Altar of Hakkar (Avatar event)")
                            .Anchored(7)
                            .WaitForSpawn(8443, /*wantAlive*/ true, /*timeout*/ 45000)
                            .Optional()
                            .Build());

            // --- ZulFarrak (map 209), event 1 ------------------------------
            // Temple-summit (Sandfury prisoner) event: reaching the summit fires
            // the scripted event that opens Chief Ukorz's door. The anchor's
            // travel + arrival already drives that today; this event row migrates
            // the objective onto the framework with NO extra steps, so behaviour
            // is unchanged (arrive => complete). TODO(milestone 2): once the
            // prisoner-release lever / gossip is verified, add UseGO/Gossip +
            // WaitForGOState here so the door open is explicit rather than
            // incidental.
            t.push_back(EventBuilder(209, 1, "Temple Summit (Executioner event)")
                            .Anchored(7)
                            .Build());

            // --- Shadowfang Keep (map 33) — CONDITIONAL, FACTION-SPECIFIC ---
            // The Courtyard Door (GO 18895) gates the keep past the entry rooms
            // and is opened only by a freed prisoner, not by the party. The real
            // mechanic (verified from the SFK SmartAI, 2026-06-11) is faction-
            // specific — each side has its OWN lever + prisoner:
            //   Alliance: pull lever 18901 (opens cell gate 18936) -> gossip
            //             Sorcerer Ashcrombe (3850, menu 21213) option 0.
            //   Horde:    pull lever 18900 (opens cell gate 18934) -> gossip
            //             Deathstalker Adamant (3849, menu 21214) option 0.
            // Picking the option fires the prisoner's GOSSIP_SELECT SmartAI: he
            // walks to the courtyard door and, ~35s later (5s + 30s waypoint
            // pauses), opens it. Two events, one per faction, each gated by a
            // team-aware condition (1 = Alliance, 2 = Horde) so the right lever +
            // prisoner drive; only one is ever due for a given party. The step
            // list is lever -> gossip -> wait-for-door (the earlier version
            // skipped the lever, so the bot could never reach the caged prisoner
            // and the gossip was a no-op). Relevance 31 (DungeonClearEventDue)
            // preempts the boss pull / door-blocked stall. Optional so a
            // non-firing script degrades to the normal door-blocked stall.
            // TODO(live-verify): per faction, lever opens the cell, gossip sends
            // the prisoner, courtyard door opens within the 60s budget.
            // After freeing the prisoner, walk up to the courtyard door and wait
            // THERE (not in the cell) for it to open — the closed door stops the
            // approach a few yards short, so the tank is parked ready to walk
            // through the instant the prisoner opens it.
            t.push_back(EventBuilder(33, 1, "Free Ashcrombe (Courtyard Door, Alliance)")
                            .Conditional(1)
                            .MoveTo(-248.0f, 2122.0f, 81.3f, /*radius*/ 6.0f)
                            .UseGO(/*lever*/ 18901, /*searchRadius*/ 14.0f)
                            .Gossip(/*Sorcerer Ashcrombe*/ 3850, /*option*/ 0, /*searchRadius*/ 16.0f)
                            .MoveTo(/*courtyard door*/ -242.58f, 2159.05f, 90.62f, /*radius*/ 9.0f)
                            .WaitForGOState(/*courtyard door*/ 18895, /*GO_STATE_ACTIVE*/ 0,
                                            /*timeout*/ 60000)
                            .Optional()
                            .Build());

            t.push_back(EventBuilder(33, 2, "Free Adamant (Courtyard Door, Horde)")
                            .Conditional(2)
                            .MoveTo(-251.0f, 2115.0f, 81.3f, /*radius*/ 6.0f)
                            .UseGO(/*lever*/ 18900, /*searchRadius*/ 14.0f)
                            .Gossip(/*Deathstalker Adamant*/ 3849, /*option*/ 0, /*searchRadius*/ 16.0f)
                            .MoveTo(/*courtyard door*/ -242.58f, 2159.05f, 90.62f, /*radius*/ 9.0f)
                            .WaitForGOState(/*courtyard door*/ 18895, /*GO_STATE_ACTIVE*/ 0,
                                            /*timeout*/ 60000)
                            .Optional()
                            .Build());

            return t;
        }();
        return kEvents;
    }
}

DungeonEvent const* DungeonEventRegistry::Find(uint32 mapId, uint32 id)
{
    if (id == 0)
        return nullptr;
    for (DungeonEvent const& e : EventTable())
        if (e.mapId == mapId && e.id == id)
            return &e;
    return nullptr;
}

bool DungeonEventRegistry::HasEvents(uint32 mapId)
{
    for (DungeonEvent const& e : EventTable())
        if (e.mapId == mapId)
            return true;
    return false;
}

std::vector<DungeonEvent const*> DungeonEventRegistry::Conditional(uint32 mapId)
{
    std::vector<DungeonEvent const*> out;
    for (DungeonEvent const& e : EventTable())
        if (e.mapId == mapId && e.activation == EventActivation::Conditional)
            out.push_back(&e);
    return out;
}
