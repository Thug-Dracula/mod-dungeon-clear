/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONEVENTEXECUTOR_H
#define _PLAYERBOT_DUNGEONEVENTEXECUTOR_H

#include "Common.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"

class Player;
class Creature;
class AiObjectContext;

// Result of running ONE event step on a tick.
enum class StepResult : uint8
{
    Running,  // still working — call again next tick
    Done,     // this step finished — advance to the next
    Blocked,  // cannot finish unaided (needs the human) — stall the run
    Failed,   // step failed / timed out — required stalls, optional skips
};

// What the driver should do after a step result is folded into progress.
enum class EventDriveOutcome : uint8
{
    Running,    // event still in progress — keep holding at the anchor
    Completed,  // all steps done — latch the anchor cleared and advance
    Stalled,    // a required step is blocked/failed — stall for the human
    Skipped,    // an optional step failed — latch cleared and advance anyway
};

// Per-run progress through one event, owned by a DungeonClear value (leader-
// keyed). Self-healing: when a different event starts (eventId mismatch) Drive
// resets it, so a stale value from a prior run is harmless.
struct DungeonEventProgress
{
    uint32 eventId{0};      // event currently being driven (0 = none)
    uint32 stepIndex{0};    // index of the active step
    uint32 stepStartMs{0};  // ms-time the active step was entered (timeout base)
    uint32 attempts{0};     // per-step attempt counter (re-click cadence, etc.)
    uint32 lastDriveMs{0};  // ms-time Drive last ran this event (gap detector)
    uint32 instanceId{0};   // instance this progress belongs to (new-instance reset)

    // EscortCreature watchdog: ms-time the escort last made genuine progress
    // (escortee moved, combat occurred, a reachable threat existed, or the final
    // boss is pending within grace). The escort step has no flat timeout (the
    // 32.5s banish channel + the long ritual would mis-fire one); this is its
    // dead-air liveness clock instead. 0 => unset (stamped on the first tick).
    uint32 escortProgressMs{0};

    // Drive-log throttle: the per-tick step line is logged only on a transition
    // (step or result change) or every kLogHeartbeatMs while Running, so a long
    // WaitForSpawn doesn't spam one line per tick.
    int32  lastLoggedStep{-1};
    int32  lastLoggedResult{-1};
    uint32 lastLogMs{0};

    void Reset()
    {
        eventId = 0;
        stepIndex = 0;
        stepStartMs = 0;
        attempts = 0;
        lastDriveMs = 0;
        instanceId = 0;
        escortProgressMs = 0;
        lastLoggedStep = -1;
        lastLoggedResult = -1;
        lastLogMs = 0;
    }
};

class DungeonEventExecutor
{
public:
    // PURE state transition: fold a step's `result` into `prog` and report what
    // the driver should do. Handles step advancement, the per-step timeout
    // (escalates a too-long Running to Failed using `nowMs - stepStartMs`), and
    // the required/optional terminal mapping. No game state — unit-tested
    // directly. `defaultTimeoutMs` is used when the step's own timeoutMs is 0.
    static EventDriveOutcome Advance(DungeonEvent const& ev, DungeonEventProgress& prog,
                                     StepResult result, uint32 nowMs, uint32 defaultTimeoutMs);

    // IMPURE driver: (re)initialise progress for `ev`, run the active step
    // against the live bot/world, and Advance(). Returns the driver outcome.
    static EventDriveOutcome Drive(Player* bot, AiObjectContext* context,
                                   DungeonEvent const& ev, DungeonEventProgress& prog);

    // IMPURE: run a single step against the live world. Exposed for the driver;
    // separated from Advance so the latter stays pure/testable.
    static StepResult RunStep(Player* bot, AiObjectContext* context,
                              EventStep const& step, DungeonEventProgress& prog, uint32 nowMs);

    // True once the leader has fallen onto a DropInHole step's deep-floor landing
    // (settled at/below landing Z and no longer falling). Shared by RunStep's gate
    // and the action's DriveDropInHole so the "still dropping vs. landed" decision
    // is single-sourced. Z-based: the MoveFall is pure-vertical, so the leader's
    // X/Y is already over the landing — only the descent has to finish.
    static bool IsOnDropLanding(Player* bot, EventStep const& step);

    // IMPURE: drive the gossip OPCODES to open `npc`'s menu and select `option`,
    // returning true once the select has been sent (false while the menu/option
    // is not yet populated). Shared by the Gossip step and the EscortCreature
    // step's self-heal start (DriveEscortCreature), so the one subtle bit — the
    // core rejects a select whose packet guid isn't the open menu's sender, so we
    // send the NPC's OWN guid rather than the master's target — lives in one
    // place. The caller is responsible for being in interact range and (if it
    // matters) facing the NPC.
    static bool SelectGossip(Player* bot, Creature* npc, int32 option);

    // --- Conditional activation (milestone 2) ----------------------------

    // Synthetic "dungeon clear cleared anchors" latch key for a Conditional
    // event of `eventId`. Conditional events have no boss-list anchor entry, so
    // they latch under a key in a high range that can never collide with a real
    // creature/anchor entry. Keep this pure so the trigger, action and tests all
    // agree on the key.
    static constexpr uint32 ConditionalLatchKey(uint32 eventId)
    {
        return 0x7F000000u + eventId;
    }

    // IMPURE: the first un-latched Conditional event registered for `mapId`
    // whose EventConditionRegistry predicate is currently true; nullptr if none
    // is due. Shared by the conditional-event trigger (gate) and DcRunEventAction
    // (driver) so the two never disagree about which event is active.
    static DungeonEvent const* FindDueConditionalEvent(Player* bot, AiObjectContext* context,
                                                       uint32 mapId);

    // IMPURE: detect conditional events whose completion is signalled by their
    // own gating condition going false (instance state, not a ConditionalLatchKey)
    // and latch them into "dungeon clear cleared anchors" so the panel can show
    // them done. Some events (e.g. Stratholme ziggurat acolyte clears) flip their
    // instance data 1 -> 2 during combat, before the dormant in-combat executor
    // can run its own completion tick, so they never latch the normal way. Using
    // "dungeon clear seen due events", an event that was once due and now reads
    // not-due (and is neither repeatable nor a per-boss room-aggro pre-clear) is
    // treated as complete. Called from the throttled status-publisher tick so it
    // runs regardless of any bot's combat state. No-op when the map has no
    // conditional events.
    static void SweepCompletedConditionalEvents(Player* bot, AiObjectContext* context,
                                                uint32 mapId);

    // True if `context`'s run is currently driving a PERSISTENT anchored event
    // that has started (stepIndex past 0) — i.e. a long multi-phase set-piece
    // (ZulFarrak's temple) owns the tank. Single source of truth used to stand
    // other systems down for the event's whole duration:
    //   - the PULL pipeline (no advanced-pull camp-drag mid-event),
    //   - the follower SCOUT-LAG (followers stay tight on the tank instead of
    //     lagging up-ramp to rest and arriving late for the next wave), and
    //   - the at-objective trigger stays sticky (tank may roam from the anchor).
    // Reads the run's "next dungeon boss" + "dungeon clear event progress" values,
    // so pass the context of the bot whose run state you mean (the leader's).
    static bool IsPersistentAnchoredEventActive(AiObjectContext* context);
};

#endif
