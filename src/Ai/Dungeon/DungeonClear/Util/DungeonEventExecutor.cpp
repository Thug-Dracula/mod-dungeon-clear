/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonEventExecutor.h"

#include <unordered_set>

#include "Creature.h"
#include "GameObject.h"
#include "GossipHelloAction.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/EventConditionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

namespace
{
    // Default arrival radius for an event MoveTo when the step doesn't override
    // it; kept tighter than the objective arrive default since these are usually
    // intra-room hops to a specific interactable.
    constexpr float DC_EVENT_MOVE_RADIUS = 4.0f;
    // How far out to look for a step's GameObject when no search radius is given.
    constexpr float DC_EVENT_GO_SEARCH = 20.0f;
    // Range from which a GameObject may legitimately be Use()d (it has no range
    // check of its own — same rule the door system enforces).
    constexpr float DC_EVENT_GO_USE_RANGE = 5.0f;
    // How far out WaitForSpawn / KillCreature scan for the named creature.
    constexpr float DC_EVENT_CREATURE_SCAN = 250.0f;
    // Range from which a creature can be gossiped (mirrors the core's
    // GetNPCIfCanInteractWith INTERACTION_DISTANCE check; kept a hair tighter).
    constexpr float DC_EVENT_GOSSIP_RANGE = 5.0f;

    // Issue a one-shot move toward (x,y,z) if not already moving there. Short,
    // intra-room hops — the surrounding objective travel got the party into the
    // room; this just closes the last few yards to an interactable.
    void HopTo(Player* bot, float x, float y, float z)
    {
        if (bot->isMoving())
            return;
        bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          /*generatePath*/ true, false);
    }
}

StepResult DungeonEventExecutor::RunStep(Player* bot, AiObjectContext* context,
                                         EventStep const& step, DungeonEventProgress& prog,
                                         uint32 nowMs)
{
    if (!bot)
        return StepResult::Blocked;

    switch (step.kind)
    {
        case EventStepKind::MoveTo:
        {
            float const radius = step.radius > 0.0f ? step.radius : DC_EVENT_MOVE_RADIUS;
            if (bot->GetExactDist(step.x, step.y, step.z) <= radius)
                return StepResult::Done;
            HopTo(bot, step.x, step.y, step.z);
            return StepResult::Running;
        }

        case EventStepKind::UseGameObject:
        {
            float const search = step.radius > 0.0f ? step.radius : DC_EVENT_GO_SEARCH;
            GameObject* go = bot->FindNearestGameObject(step.goEntry, search);
            if (!go)
                return StepResult::Running;  // not in range yet / not spawned
            if (!bot->IsWithinDistInMap(go, DC_EVENT_GO_USE_RANGE))
            {
                HopTo(bot, go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
                return StepResult::Running;
            }
            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] {} event-step Use GO {} '{}'",
                      bot->GetName(), go->GetGUID().ToString(), go->GetName());
            go->Use(bot);
            return StepResult::Done;
        }

        case EventStepKind::WaitForSpawn:
        {
            Creature* c = bot->FindNearestCreature(step.creatureEntry, DC_EVENT_CREATURE_SCAN,
                                                   /*alive*/ step.wantAlive);
            bool const present = c != nullptr;
            // wantAlive: done once it's up. !wantAlive: done once it's gone.
            return (present == step.wantAlive) ? StepResult::Done : StepResult::Running;
        }

        case EventStepKind::WaitForGameObjectState:
        {
            float const search = step.radius > 0.0f ? step.radius : DC_EVENT_GO_SEARCH;
            GameObject* go = bot->FindNearestGameObject(step.goEntry, search);
            if (!go)
                return StepResult::Running;
            return (static_cast<uint32>(go->GetGoState()) == step.wantState)
                       ? StepResult::Done
                       : StepResult::Running;
        }

        case EventStepKind::KillCreature:
        {
            // Gate only: the executor never engages (the surrounding objective
            // action holds position). Killing is the engage pipeline's job — this
            // step just reports Done once the named creature is no longer alive
            // nearby. Used by the conditional room-aggro fold-in (milestone 3),
            // where the event preempts above the engage triggers so the party
            // actually fights. `count` is approximated as "any alive blocks".
            float const search = step.radius > 0.0f ? step.radius : DC_EVENT_CREATURE_SCAN;
            Creature* c = bot->FindNearestCreature(step.creatureEntry, search, /*alive*/ true);
            return c ? StepResult::Running : StepResult::Done;
        }

        case EventStepKind::Wait:
        {
            return (getMSTimeDiff(prog.stepStartMs, nowMs) >= step.durationMs)
                       ? StepResult::Done
                       : StepResult::Running;
        }

        case EventStepKind::Custom:
        {
            DungeonBossInfo dummy;  // legacy hooks key off bot/context, not info
            ObjectiveArriveResult const r =
                ObjectiveHookRegistry::Run(step.hookId, bot, context, dummy);
            switch (r)
            {
                case ObjectiveArriveResult::Done:    return StepResult::Done;
                case ObjectiveArriveResult::Running: return StepResult::Running;
                case ObjectiveArriveResult::Blocked: return StepResult::Blocked;
            }
            return StepResult::Done;
        }

        case EventStepKind::Gossip:
        {
            // Talk to creatureEntry and pick gossipOption. The select must run
            // against an already-open menu (GossipHelloAction populates the bot's
            // PlayerTalkClass on the -1 "hello" call, then ProcessGossip reads
            // that menu), so this is a two-touch step: attempt 0 opens the menu,
            // the next attempt selects. The select's downstream effect (a gate
            // opening, a spawn) is confirmed by a following WaitForGOState /
            // WaitForSpawn step, not here — this step is Done once the option is
            // sent. Reuses mod-playerbots' proven GossipHelloAction path.
            float const search = step.radius > 0.0f ? step.radius : DC_EVENT_GO_SEARCH;
            Creature* npc = bot->FindNearestCreature(step.creatureEntry, search, /*alive*/ true);
            if (!npc)
                return StepResult::Running;  // walking in / not spawned yet

            if (!bot->IsWithinDistInMap(npc, DC_EVENT_GOSSIP_RANGE))
            {
                HopTo(bot, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
                return StepResult::Running;
            }

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return StepResult::Blocked;

            GossipHelloAction gossip(botAI);
            if (prog.attempts == 0)
            {
                // Open the menu; select on the next tick against the open menu.
                gossip.Execute(npc->GetGUID(), -1, /*silent*/ true);
                ++prog.attempts;
                return StepResult::Running;
            }

            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] {} event-step Gossip {} '{}' option {}",
                      bot->GetName(), npc->GetGUID().ToString(), npc->GetName(),
                      step.gossipOption);
            if (gossip.Execute(npc->GetGUID(), step.gossipOption, /*silent*/ true))
                return StepResult::Done;
            // Select failed (menu went stale / option missing) — re-open and
            // retry next tick. The Advance timeout escalates if it keeps failing.
            prog.attempts = 0;
            return StepResult::Running;
        }

        case EventStepKind::CastSpell:
        case EventStepKind::UseItem:
        default:
            // Not yet implemented (milestone 3+). Blocked makes an accidentally
            // authored step stall visibly rather than silently pass.
            LOG_WARN("playerbots.dungeonclear",
                     "[dungeon-clear] {} event-step kind {} not yet implemented",
                     bot->GetName(), static_cast<uint32>(step.kind));
            return StepResult::Blocked;
    }
}

EventDriveOutcome DungeonEventExecutor::Advance(DungeonEvent const& ev, DungeonEventProgress& prog,
                                                StepResult result, uint32 nowMs,
                                                uint32 defaultTimeoutMs)
{
    if (prog.stepIndex >= ev.steps.size())
        return EventDriveOutcome::Completed;

    EventStep const& step = ev.steps[prog.stepIndex];
    uint32 const timeout = step.timeoutMs ? step.timeoutMs : defaultTimeoutMs;

    if (result == StepResult::Running)
    {
        // Escalate a step that has run too long to Failed. uint32 subtraction is
        // wrap-safe for an elapsed interval.
        if (timeout > 0 && static_cast<uint32>(nowMs - prog.stepStartMs) >= timeout)
            result = StepResult::Failed;
        else
            return EventDriveOutcome::Running;
    }

    if (result == StepResult::Done)
    {
        ++prog.stepIndex;
        prog.attempts = 0;
        prog.stepStartMs = nowMs;
        return (prog.stepIndex >= ev.steps.size()) ? EventDriveOutcome::Completed
                                                   : EventDriveOutcome::Running;
    }

    if (result == StepResult::Blocked)
        return EventDriveOutcome::Stalled;

    // Failed (or timed out): required events stall for the human; optional ones
    // skip the rest of the event and let the clear advance.
    return ev.required ? EventDriveOutcome::Stalled : EventDriveOutcome::Skipped;
}

EventDriveOutcome DungeonEventExecutor::Drive(Player* bot, AiObjectContext* context,
                                              DungeonEvent const& ev, DungeonEventProgress& prog)
{
    uint32 const now = getMSTime();

    // (Re)initialise on a new event — self-heals a stale value from a prior run.
    if (prog.eventId != ev.id)
    {
        prog.eventId = ev.id;
        prog.stepIndex = 0;
        prog.attempts = 0;
        prog.stepStartMs = now;
    }
    // Resuming the SAME event after a gap (a prior session left this value with
    // eventId already == ev.id, or the event was interrupted by combat/looting
    // for a while): the step clock is stale, so refresh it — otherwise the very
    // next Advance escalates the current step to a timeout against an ancient
    // stepStartMs and (for an optional event) skips it on the first tick. Keep
    // stepIndex so we don't redo a non-idempotent step (e.g. re-pull a lever and
    // toggle the gate shut again); just give the current step a fresh budget.
    else if (getMSTimeDiff(prog.lastDriveMs, now) > 1000)
    {
        prog.stepStartMs = now;
        prog.attempts = 0;
    }
    prog.lastDriveMs = now;

    if (prog.stepIndex >= ev.steps.size())
        return EventDriveOutcome::Completed;

    EventStep const& active = ev.steps[prog.stepIndex];
    StepResult const result = RunStep(bot, context, active, prog, now);

    uint32 const defaultTimeoutMs =
        bot ? DcSettings::GetUInt(bot, "EventStepTimeout") * 1000u : 30000u;

    if (bot)
    {
        uint32 const timeout = active.timeoutMs ? active.timeoutMs : defaultTimeoutMs;
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] event '{}' step {} kind {} result {} elapsed {}ms timeout {}ms",
                  bot->GetName(), ev.name, prog.stepIndex,
                  static_cast<uint32>(active.kind), static_cast<uint32>(result),
                  static_cast<uint32>(now - prog.stepStartMs), timeout);
    }

    return Advance(ev, prog, result, now, defaultTimeoutMs);
}

DungeonEvent const* DungeonEventExecutor::FindDueConditionalEvent(Player* bot,
                                                                  AiObjectContext* context,
                                                                  uint32 mapId)
{
    if (!bot || !context)
        return nullptr;

    std::vector<DungeonEvent const*> const conditional = DungeonEventRegistry::Conditional(mapId);
    if (conditional.empty())
        return nullptr;

    auto const& cleared =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear cleared anchors")->Get();

    for (DungeonEvent const* ev : conditional)
    {
        // Already done this run — its synthetic latch key is set.
        if (cleared.count(ConditionalLatchKey(ev->id)))
            continue;
        if (EventConditionRegistry::Evaluate(ev->conditionId, bot, context))
            return ev;
    }
    return nullptr;
}
