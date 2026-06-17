/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONEVENTREGISTRY_H
#define _PLAYERBOT_DUNGEONEVENTREGISTRY_H

#include <string>
#include <vector>

#include "Common.h"

// Declarative framework for scripted dungeon EVENTS the party must perform to
// progress — pull a lever, click an altar, talk to an NPC and pick a gossip
// option, walk to a spot to trigger a spawn, free a prisoner, etc. An event is
// PURE DATA: an ordered list of typed steps the DungeonEventExecutor drives one
// at a time. This replaces hand-written C++ in ObjectiveHookRegistry with a
// reusable step vocabulary (the old freeform hook survives as the `Custom` step).
//
// Authoring mirrors the other DungeonClear registries: a hardcoded table in a
// GLOB'd .cpp (link-safe in the static-lib build). See DungeonEventRegistry.cpp.
//
// Milestone 1 ships the anchored model + the low-risk steps. An anchored event
// is referenced by a travel-objective anchor (DungeonBossInfo::eventId, kind
// Objective): when the tank reaches the anchor the executor runs the event's
// steps to completion before the clear advances. Gossip and the conditional
// activation model land in later milestones (the enum carries their slots now).

enum class EventStepKind : uint8
{
    MoveTo,                  // walk the leader to (x,y,z) within `radius` — a SHORT
                             // intra-room hop only (plain MovePoint). A far haul
                             // must be done by a BOSS/OBJECTIVE anchor so the boss
                             // navigation + dynamic-pull machinery drives it (an
                             // event step driving a long move fights the pull camp).
    Jump,                    // ballistic MoveJump to (x,y,z) within `radius` — bridge
                             // a navmesh gap a ground move can't (a drop-down ledge
                             // onto a disconnected mesh island). Same SHORT-hop rule
                             // as MoveTo: the anchor navigation gets the bot to the
                             // lip; this clears the one off-mesh leg. Idempotent —
                             // Done once landed (within radius), so a Drive restart
                             // after a combat tick-gap re-runs it safely.
    UseGameObject,           // approach + GameObject::Use(bot) the nearest goEntry
    Gossip,                  // talk creatureEntry + select gossipOption (milestone 2)
    WaitForSpawn,            // hold until creatureEntry is alive (wantAlive) / gone
    WaitForGameObjectState,  // hold until goEntry reaches GOState `wantState`
    KillCreature,            // gate: done when no alive creatureEntry remain in range
    ClearRadius,             // engage + gate: clear every reachable hostile within
                             // `radius` (2D) and a floor `zBand` of (x,y,z); done
                             // when none remain. POSITION-based (any entry), unlike
                             // KillCreature — a point-anchored room pre-clear.
    CastSpell,               // leader casts spellId on self (milestone 2+)
    UseItem,                 // leader uses itemId (milestone 2+)
    Wait,                    // dwell `durationMs` then continue
    Custom,                  // escape hatch -> ObjectiveHookRegistry hookId
};

// One typed primitive. Fields are a shared bag — only those relevant to `kind`
// are read (see DungeonEventExecutor::RunStep). Authoring goes through the
// fluent EventBuilder below, not by populating this directly.
struct EventStep
{
    EventStepKind kind{EventStepKind::Wait};

    // Geometry: MoveTo target, and the search anchor for GO/creature steps when
    // (x,y,z) is set; (0,0,0) means "search around the bot".
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float radius{0.0f};      // arrival / search radius; 0 => a per-kind default
    float zBand{0.0f};       // ClearRadius: vertical half-band around z (keeps a
                             // multi-level chamber's balconies/pit out of a clear)

    uint32 goEntry{0};       // UseGameObject / WaitForGameObjectState
    uint32 creatureEntry{0}; // Gossip target / WaitForSpawn / KillCreature
    uint32 spellId{0};       // CastSpell
    uint32 itemId{0};        // UseItem

    int32  gossipOption{-1}; // Gossip: option index to select (-1 = none)
    uint32 count{1};         // KillCreature: alive-count that still blocks
    uint32 wantState{0};     // WaitForGameObjectState: target GOState value
    bool   wantAlive{true};  // WaitForSpawn: wait for alive (true) vs gone (false)

    // KillCreature only. When set, the DRIVING action (DcObjectiveArriveAction /
    // DcRunEventAction) actively walks the leader into the named creature via the
    // engage pipeline (EngageDirect — long-range path + combat) instead of merely
    // GATING on its death while the bot holds. Use it for a kill the bot must seek
    // out (a boss across the room / down the stairs), where waiting for the mob to
    // come to a held bot would deadlock. The step is still Done once no live
    // creature of the entry remains. (Distinct from the room-aggro entry-0 mode,
    // which targets the room-trash value rather than a fixed entry.)
    bool   engage{false};

    // Gossip only. When set, a Gossip step whose target creature is not alive
    // anywhere nearby is SKIPPED (Done) instead of waited on — for an OPTIONAL
    // interaction the run must not deadlock on if the NPC died (a freed ZulFarrak
    // helper the party let die: a dead Weegli leaves the boss unreachable but we
    // still talk to Bly, and if every helper is dead the event simply completes
    // and the clear continues). A wide rescan separates "dead/gone" (skip) from
    // "still walking in / just outside the gossip search radius" (keep approaching).
    bool   skipIfMissing{false};

    // Gossip only. When set, the step WAITS (Running) while the target creature is
    // still moving, so the bot never talks to an NPC mid-walk. Used when a scripted
    // NPC walks to a final position before its gossip is meaningful (ZulFarrak's
    // freed crew descending to the temple floor — their gossip is already offered
    // while they are still walking down). Combined with skipIfMissing, a dead NPC
    // still skips rather than waiting forever.
    bool   waitForStill{false};

    uint32 durationMs{0};    // Wait: dwell length
    uint32 timeoutMs{0};     // 0 => EventStepTimeout config default; else per-step
    uint32 hookId{0};        // Custom -> ObjectiveHookRegistry

    // MoveTo garrison gate, instance-data variant. When instanceDataId >= 0 the
    // step holds at (x,y,z) until the map's InstanceScript GetData(instanceDataId)
    // reaches instanceDataMin. Preferred over the creatureEntry "until alive" gate
    // for a value that the party kills DURING continuous combat (the event engine
    // is dormant in combat, so a transient "boss alive" window can be missed and
    // never recovers once the boss dies): a scripted phase counter only ever
    // climbs, so a >= gate is safe to observe late. -1 => no instance-data gate.
    int32  instanceDataId{-1};
    uint32 instanceDataMin{0};
};

// How an event enters the clear. Milestone 1 only uses Anchored; Conditional is
// reserved for off-path levers / pre-boss gates (milestone 2).
enum class EventActivation : uint8
{
    Anchored,     // referenced by a boss-list objective anchor (orderIndex)
    Conditional,  // fired by a predicate (conditionId) each tick
};

struct DungeonEvent
{
    uint32 mapId{0};
    uint32 id{0};            // stable per-map id (objective eventId / latch key)
    std::string name;

    EventActivation activation{EventActivation::Anchored};
    uint32 orderIndex{0};    // Anchored: the objective's encounter slot (doc only)
    uint32 conditionId{0};   // Conditional: predicate id (milestone 2)

    std::vector<EventStep> steps;

    // Required => a Blocked/timed-out step stalls the run for the human;
    // Optional => such a step is skipped and the clear advances anyway (used for
    // best-effort events whose scripted trigger may not fire for bots).
    bool required{true};

    // Conditional events only. A normal conditional event latches DONE the moment
    // its step list completes (DcRunEventAction inserts its ConditionalLatchKey),
    // so it never fires again that run. A Repeatable event is NOT latched on
    // completion — it re-fires every time its condition reads true again. Use it
    // for an action the party must repeat an unknown number of times, where some
    // OTHER live signal (not a one-shot latch) gates each repeat and ends the
    // loop. Example: Razorfen Downs' gong, which must be rung once per wave until
    // the boss spawns — the gong's own selectable flag gates each ring and the
    // boss going live ends the loop, so a fixed latch would stop it after ring 1.
    bool repeatable{false};

    // ANCHORED events only. A normal anchored event is driven only while the tank
    // sits at its objective anchor, and DungeonEventExecutor::Drive RESTARTS it
    // from step 0 whenever Drive hasn't run for >1s (treated as a fresh
    // activation). That is right for a quick one-room event, but wrong for a long
    // multi-combat event that spans the dungeon (ZulFarrak's temple): every wave /
    // boss fight is a >1s gap on the combat engine, which would rewind the step
    // list each time combat ends. A Persistent event keeps its progress across
    // those gaps — Drive only (re)initialises it on a real eventId mismatch (new
    // run / different event), never on a tick gap — and its "at objective" trigger
    // stays sticky once started so the tank can roam far from the anchor (down the
    // stairs to the bosses) while the event drives, instead of being held at it.
    bool persistent{false};

    // Conditional events only, panel cosmetics. By default an off-path
    // conditional event renders last in the `dc bosses` panel (index 99). When
    // this names a boss entry, the event instead sorts just BEFORE that boss —
    // for a prerequisite the party performs before the boss is reachable (e.g.
    // the gong before Tuten'kash). 0 => default (sort last). Does not affect
    // engine ordering, only the status panel.
    uint32 panelGatesBossEntry{0};
};

// Fluent builder for readable registry rows, mirroring MakeBoss/MakeObjective.
//
//   Event(mapId, id, "Free the prisoner")
//       .MoveTo(x, y, z, 12)
//       .UseGO(GO_LEVER, 10)
//       .WaitForGOState(GO_GATE, GO_STATE_ACTIVE, 8000)
//       .Build();
class EventBuilder
{
public:
    EventBuilder(uint32 mapId, uint32 id, std::string name);

    EventBuilder& Anchored(uint32 orderIndex);
    EventBuilder& Conditional(uint32 conditionId);
    EventBuilder& Optional();
    EventBuilder& Repeatable();
    EventBuilder& Persistent();
    EventBuilder& PanelBeforeBoss(uint32 bossEntry);

    // Override the LAST-added step's timeout (0 on the step => the
    // EventStepTimeout config default). Chain it right after the step it tunes:
    //   .WaitForSpawn(entry).Timeout(900000)
    EventBuilder& Timeout(uint32 ms);

    // Mark the LAST-added step's target as optional: a Gossip step whose creature
    // is dead/gone is skipped rather than waited on. Chain after the step:
    //   .Gossip(npc, 0).SkipIfTargetMissing()
    EventBuilder& SkipIfTargetMissing();

    // Make the LAST-added Gossip step wait until its target NPC stops moving (has
    // finished a scripted walk) before talking to it. Chain after the step:
    //   .Gossip(npc, 0).WaitTargetStill()
    EventBuilder& WaitTargetStill();

    EventBuilder& MoveTo(float x, float y, float z, float radius = 0.0f);
    // Ballistic jump to (x,y,z): MotionMaster::MoveJump at run speed. Bridges a
    // navmesh gap a ground move can't (drop-down ledge / off-mesh island). The
    // preceding anchor/MoveTo must place the bot on the jump LIP; this clears the
    // gap. Done once the bot is within `radius` of the landing.
    EventBuilder& Jump(float x, float y, float z, float radius = 4.0f);
    // MoveTo that GARRISONS: walk to (x,y,z) and HOLD there — re-moving back if a
    // later tick finds the bot displaced (combat pushed it off the spot) — until
    // `creatureEntry` matches `wantAlive`. Keeps the tank anchored at a staging
    // point between waves (ZulFarrak's ramp head) instead of holding wherever the
    // last fight ended (down on the wave spawn). Internally a MoveTo step with a
    // spawn gate; creatureEntry 0 would be a plain one-shot MoveTo.
    EventBuilder& MoveToHoldUntilSpawn(float x, float y, float z, float radius,
                                       uint32 creatureEntry, bool wantAlive = true);
    // Garrison variant gated on a monotonic InstanceScript phase counter: hold at
    // (x,y,z) until GetData(dataId) >= minValue. Robust where the party fights
    // through the gate during continuous combat (ZulFarrak's temple — hold the
    // ramp until DATA_PYRAMID reaches WAVE_3, then descend; works even if the
    // bosses are already dead by the time the party drops combat).
    EventBuilder& MoveToHoldUntilInstanceData(float x, float y, float z, float radius,
                                              uint32 dataId, uint32 minValue);
    EventBuilder& UseGO(uint32 goEntry, float searchRadius = 0.0f,
                        float x = 0.0f, float y = 0.0f, float z = 0.0f);
    // Leader casts `spellId` on itself (triggered: no cost/cooldown/reagent/cast
    // time). For a scripted "use a quest item" spell whose effect a bot cannot
    // otherwise reach — e.g. Sunken Temple's "Awaken the Soulflayer" (12346),
    // which a player normally fires via Yeh'kinya's Scroll / Egg of Hakkar to
    // summon the Shade of Hakkar. The spell's SEND_EVENT script only checks the
    // instance + event-not-started, so a direct triggered cast summons the Shade
    // without the quest item (and no-ops if the event already started).
    EventBuilder& CastSpell(uint32 spellId);
    // Leader USES `itemId` (granted first if not in bags), via the full item-use
    // cast path. For a scripted "use a quest item" the bot never earned — Sunken
    // Temple's Egg of Hakkar (10465), which fires Awaken the Soulflayer to summon
    // the Shade ONLY when used as an item (a bare CastSpell is rejected).
    EventBuilder& UseItem(uint32 itemId);
    EventBuilder& Gossip(uint32 creatureEntry, int32 option, float searchRadius = 0.0f);
    EventBuilder& WaitForSpawn(uint32 creatureEntry, bool wantAlive = true,
                               uint32 timeoutMs = 0);
    EventBuilder& WaitForGOState(uint32 goEntry, uint32 wantState,
                                 uint32 timeoutMs = 0, float searchRadius = 0.0f);
    EventBuilder& KillCreature(uint32 creatureEntry, uint32 count = 1,
                               float searchRadius = 0.0f);
    // KillCreature variant whose driving action actively seeks out and engages
    // the creature (EngageDirect) rather than only gating on its death. See
    // EventStep::engage.
    EventBuilder& KillCreatureEngage(uint32 creatureEntry, uint32 count = 1,
                                     float searchRadius = 0.0f);
    // Point-anchored room pre-clear: the driving action engages every reachable
    // hostile within `radius` (2D) and `zBand` (vertical, floor-keeping) of the
    // centre (x,y,z); the step is Done once none remain. Use on an OBJECTIVE
    // anchor placed at the centre so boss-nav travels the tank in first.
    EventBuilder& ClearRadius(float x, float y, float z, float radius,
                              float zBand = 20.0f);
    EventBuilder& Wait(uint32 durationMs);
    EventBuilder& Custom(uint32 hookId);

    DungeonEvent Build() const { return _ev; }

private:
    EventStep& Add(EventStepKind kind);
    DungeonEvent _ev;
};

class DungeonEventRegistry
{
public:
    // The event with `id` on `mapId`, or nullptr if none is registered.
    static DungeonEvent const* Find(uint32 mapId, uint32 id);

    // True if the map has any registered event (cheap gate for callers).
    static bool HasEvents(uint32 mapId);

    // Every Conditional-activation event registered for `mapId`, in table order.
    // Empty for maps with only Anchored events. Used by the conditional-event
    // trigger/action to find a due off-path event each tick (milestone 2).
    static std::vector<DungeonEvent const*> Conditional(uint32 mapId);

    // --- Room-aggro pre-clear (milestone 3) ------------------------------

    // True if `ev` is a room-aggro PRE-CLEAR event: a Conditional gate whose lone
    // KillCreature step uses creatureEntry 0 ("room-trash" mode — the target set
    // is resolved live through DungeonClearRoomTrashValue / the room-trash
    // targeting seam, not a fixed entry). These re-express the RoomAggroRegistry
    // bosses on the events framework: the condition (room trash remains) gates the
    // boss pull and DcRunEventAction drives the engage (nearest room trash first).
    // They are NEVER latched — the gate re-fires for each room-aggro boss on the
    // map (the condition simply reads false once a room is clear).
    static bool IsRoomAggroPreClear(DungeonEvent const& ev);

    // True if `mapId` has a room-aggro pre-clear event authored. The legacy
    // standalone room-clear trigger (relevance 26) stands down for such maps so
    // the conditional-event path (relevance 31) is the single room-clear driver.
    static bool HasRoomAggroEvent(uint32 mapId);
};

#endif
