/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONEVENTEXECUTOR_H
#define _PLAYERBOT_DUNGEONEVENTEXECUTOR_H

#include "Common.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"

class Player;
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

    void Reset()
    {
        eventId = 0;
        stepIndex = 0;
        stepStartMs = 0;
        attempts = 0;
        lastDriveMs = 0;
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
};

#endif
