/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonEventExecutor.h"

#include <optional>
#include <unordered_set>

#include "Creature.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GossipDef.h"
#include "InstanceScript.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "Player.h"
#include "Playerbots.h"
#include "Spell.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
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
            if (bot->GetExactDist(step.x, step.y, step.z) > radius)
            {
                HopTo(bot, step.x, step.y, step.z);
                return StepResult::Running;
            }
            // Arrived. A plain MoveTo (no gate) is done. A GARRISON MoveTo holds
            // here until its gate clears — and because distance is re-checked every
            // tick, a later tick that finds the bot displaced (combat pushed the
            // tank off the spot, e.g. chasing a wave down the ramp) re-moves it back.
            //
            // Instance-data gate (preferred for a value killed mid-combat): hold
            // until the map's scripted phase counter reaches the threshold. This is
            // MONOTONIC, so unlike "boss alive" it can't be missed while the event
            // engine is dormant in combat — once the phase climbs past the gate it
            // stays past it, so the step clears the moment the party next ticks
            // out of combat (even if the gated content is already done).
            if (step.instanceDataId >= 0)
            {
                InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
                uint32 const v = inst ? inst->GetData(static_cast<uint32>(step.instanceDataId)) : 0;
                return (v >= step.instanceDataMin) ? StepResult::Done : StepResult::Running;
            }
            // Creature gate: hold until the gate creature matches wantAlive.
            if (step.creatureEntry != 0)
            {
                Creature* c = bot->FindNearestCreature(step.creatureEntry,
                                                       DC_EVENT_CREATURE_SCAN,
                                                       /*alive*/ step.wantAlive);
                return ((c != nullptr) == step.wantAlive) ? StepResult::Done
                                                          : StepResult::Running;
            }
            return StepResult::Done;
        }

        case EventStepKind::UseGameObject:
        {
            float const search = step.radius > 0.0f ? step.radius : DC_EVENT_GO_SEARCH;
            GameObject* go = bot->FindNearestGameObject(step.goEntry, search);
            if (!go)
                return StepResult::Running;  // not in range yet / not spawned
            // Idempotent: a lever/door already in a non-READY (activated) state
            // has been used — toggling it again would undo it (re-close the cell
            // gate). Treat as already done so a restart of the step chain is safe.
            if (go->GetGoState() != GO_STATE_READY)
                return StepResult::Done;
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

        case EventStepKind::ClearRadius:
        {
            // Gate only, like KillCreature: Running while any reachable hostile
            // remains inside the point's radius/floor band, Done once none do. The
            // engage itself is driven by DcObjectiveArriveAction (engage pipeline).
            // Evaluated only once the tank is AT the objective (the arrive trigger
            // gates this), so the in-radius creatures are loaded — no premature
            // "clear" while still travelling in.
            float const r = step.radius > 0.0f ? step.radius : 50.0f;
            Unit* u = DcTargeting::NearestHostileNearPoint(bot, context, step.x, step.y,
                                                           step.z, r, step.zBand);
            return u ? StepResult::Running : StepResult::Done;
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
            // Talk to creatureEntry and pick gossipOption. We drive the gossip
            // OPCODES directly rather than GossipHelloAction::Execute, because its
            // select path (ProcessGossip) sends GetMaster()->GetTarget() as the
            // packet guid — and the core's HandleGossipSelectOptionOpcode rejects
            // the select unless that guid equals the open menu's sender GUID. For
            // a real bot whose master isn't targeting this NPC, the select is a
            // silent no-op (the prisoner never gets the GOSSIP_SELECT and never
            // opens the door). Sending the NPC's own GUID makes it land.
            float const search = step.radius > 0.0f ? step.radius : DC_EVENT_GO_SEARCH;
            Creature* npc = bot->FindNearestCreature(step.creatureEntry, search, /*alive*/ true);
            if (!npc)
            {
                // Optional target: if it's not merely outside the gossip search
                // radius but actually gone (no alive one anywhere nearby), SKIP the
                // step rather than waiting forever for an NPC that will never come
                // (a freed ZulFarrak helper the party let die). The wide rescan
                // separates "dead/gone" (skip) from "still walking in" (approach).
                if (step.skipIfMissing &&
                    !bot->FindNearestCreature(step.creatureEntry, DC_EVENT_CREATURE_SCAN,
                                              /*alive*/ true))
                {
                    LOG_DEBUG("playerbots.dungeonclear",
                              "[dungeon-clear] {} event-step Gossip target {} gone — skipping",
                              bot->GetName(), step.creatureEntry);
                    return StepResult::Done;
                }
                return StepResult::Running;  // walking in / not spawned yet
            }

            // Wait out a scripted walk: don't talk to an NPC that is still moving
            // to its final spot (the ZulFarrak crew descending to the temple
            // floor — their gossip is offered before they arrive). Hold until it
            // settles; combat/normal ticks continue meanwhile.
            if (step.waitForStill && npc->isMoving())
            {
                LOG_DEBUG("playerbots.dungeonclear",
                          "[dungeon-clear] {} event-step Gossip target {} still moving — waiting",
                          bot->GetName(), npc->GetGUID().ToString());
                return StepResult::Running;
            }

            if (!bot->IsWithinDistInMap(npc, DC_EVENT_GOSSIP_RANGE))
            {
                HopTo(bot, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
                return StepResult::Running;
            }

            // Open the menu — synchronously populates PlayerTalkClass with this
            // NPC as the gossip-menu sender and its DB options.
            bot->SetFacingToObject(npc);
            WorldPacket hello;
            hello << npc->GetGUID();
            bot->GetSession()->HandleGossipHelloOpcode(hello);

            GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
            if (menu.GetMenuItems().empty() ||
                !menu.GetItem(static_cast<uint32>(step.gossipOption)))
                return StepResult::Running;  // menu/option not ready yet — retry

            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] {} event-step Gossip {} '{}' menu {} option {}",
                      bot->GetName(), npc->GetGUID().ToString(), npc->GetName(),
                      menu.GetMenuId(), step.gossipOption);

            WorldPacket select;
            select << npc->GetGUID() << menu.GetMenuId()
                   << static_cast<uint32>(step.gossipOption);
            select << std::string();  // no coded box
            bot->GetSession()->HandleGossipSelectOptionOpcode(select);
            return StepResult::Done;
        }

        case EventStepKind::CastSpell:
        {
            // Leader casts spellId on self, triggered (bypassing cost / cooldown /
            // reagents / cast time / item requirement). Used for a scripted
            // "use a quest item" spell whose effect a bot cannot otherwise reach
            // — Sunken Temple's "Awaken the Soulflayer" (12346), normally fired
            // via Yeh'kinya's Scroll / Egg of Hakkar to summon the Shade of
            // Hakkar. The spell's SEND_EVENT script only checks the instance +
            // event-not-started, so a direct triggered cast summons the Shade
            // without the quest item, and is idempotent (it no-ops once the event
            // has started). One-shot: cast and report Done.
            if (step.spellId == 0)
                return StepResult::Done;
            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] {} event-step CastSpell {}",
                      bot->GetName(), step.spellId);
            bot->CastSpell(bot, step.spellId, true);
            return StepResult::Done;
        }

        case EventStepKind::UseItem:
        {
            // Leader USES a quest item (granting it first if the bag lacks it —
            // bots never ran the questline that awards it). This goes through the
            // full item-use cast path (CastItemUseSpell), which supplies the
            // cast-item context the bare spell lacks: Sunken Temple's Egg of
            // Hakkar (10465) casts "Awaken the Soulflayer" (12346) ONLY when used
            // as an item — a direct CastSpell(12346) is rejected before its
            // SEND_EVENT fires (verified: the instance's TYPE_HAKKAR_EVENT stayed
            // NOT_STARTED). The summon happens at a fixed position, but the egg
            // must be used FROM the encounter room, so the anchor sits at the room
            // centre. One-shot: use and report Done.
            if (step.itemId == 0)
                return StepResult::Done;
            Item* item = bot->GetItemByEntry(step.itemId);
            if (!item)
            {
                bot->AddItem(step.itemId, 1);
                item = bot->GetItemByEntry(step.itemId);
                if (!item)
                    return StepResult::Running;  // bags full this tick — retry
            }
            LOG_INFO("playerbots.dungeonclear",
                     "[dungeon-clear] {} event-step UseItem {} (encounter trigger)",
                     bot->GetName(), step.itemId);
            SpellCastTargets targets;
            targets.SetUnitTarget(bot);
            bot->CastItemUseSpell(item, targets, 0, 0);
            return StepResult::Done;
        }

        default:
            // Not yet implemented. Blocked makes an accidentally authored step
            // stall visibly rather than silently pass.
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
    uint32 const instanceId = bot ? bot->GetInstanceId() : 0;

    // A different instance means this is a brand-new run of the dungeon (the player
    // re-entered a fresh instance), even if the SAME event id is driven again. The
    // eventId-mismatch and >1s-gap branches below both fail to catch this for a
    // PERSISTENT event (the id is unchanged and the gap reset is skipped for
    // persistent events), so without this a COMPLETED progress from the prior
    // instance would carry over and make Drive report the event done on the first
    // tick — latching its objective and skipping it (ZulFarrak's temple read as
    // already cleared after a re-enter). Tie the reset to the instance id, NOT to
    // dc on/off, so toggling dungeon-clear mid-run preserves real progress. Note:
    // instanceId 0 means "not in an instance" — don't churn the reset on it.
    if (instanceId != 0 && prog.instanceId != 0 && prog.instanceId != instanceId)
        prog.Reset();
    prog.instanceId = instanceId;

    // (Re)initialise on a new event — self-heals a stale value from a prior run.
    if (prog.eventId != ev.id)
    {
        prog.eventId = ev.id;
        prog.stepIndex = 0;
        prog.attempts = 0;
        prog.stepStartMs = now;
    }
    // A gap since the last drive means this is a FRESH activation — a new run /
    // re-enter, or the event lapsed (condition went false) and re-fired — NOT a
    // tick-to-tick continuation. Restart from step 0 so the whole chain (e.g.
    // lever -> gossip -> wait-for-door) runs again. Without this a stale value
    // left mid-event by a prior run resumes on, say, the wait-for-door step and
    // parks there forever because the lever/gossip that would open it never run.
    // Safe because the steps are idempotent (UseGameObject skips an already-
    // activated GO, MoveTo/Gossip/WaitFor* re-run harmlessly).
    //
    // EXCEPTION: a Persistent event keeps its progress across gaps. A long,
    // multi-combat anchored event (ZulFarrak's temple) sees a >1s gap after every
    // wave / boss fight (the bot is on the combat engine, so Drive isn't called),
    // and rewinding to step 0 each time would re-run the whole chain endlessly and
    // strand WaitForSpawn(wantAlive) steps whose creature has since died. The
    // eventId-mismatch reset above still re-inits it for a genuine new run.
    else if (!ev.persistent && getMSTimeDiff(prog.lastDriveMs, now) > 1000)
    {
        prog.stepIndex = 0;
        prog.attempts = 0;
        prog.stepStartMs = now;
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
        // Throttle: log only on a transition (step or result change), or every
        // kLogHeartbeatMs while a step keeps Running — a long WaitForSpawn used to
        // emit one line per tick (~240ms), burying everything else.
        constexpr uint32 kLogHeartbeatMs = 5000;
        bool const changed = prog.lastLoggedStep != static_cast<int32>(prog.stepIndex) ||
                             prog.lastLoggedResult != static_cast<int32>(result);
        bool const heartbeat = (now - prog.lastLogMs) >= kLogHeartbeatMs;
        if (changed || heartbeat)
        {
            uint32 const timeout = active.timeoutMs ? active.timeoutMs : defaultTimeoutMs;
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] event '{}' step {} kind {} result {} elapsed {}ms timeout {}ms",
                      bot->GetName(), ev.name, prog.stepIndex,
                      static_cast<uint32>(active.kind), static_cast<uint32>(result),
                      static_cast<uint32>(now - prog.stepStartMs), timeout);
            prog.lastLoggedStep = static_cast<int32>(prog.stepIndex);
            prog.lastLoggedResult = static_cast<int32>(result);
            prog.lastLogMs = now;
        }
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

bool DungeonEventExecutor::IsPersistentAnchoredEventActive(AiObjectContext* context)
{
    if (!context)
        return false;

    std::optional<DungeonBossInfo> const next =
        context->GetValue<std::optional<DungeonBossInfo>>("next dungeon boss")->Get();
    if (!next.has_value() || next->kind != DungeonAnchorKind::Objective || !next->eventId)
        return false;

    DungeonEvent const* ev = DungeonEventRegistry::Find(next->mapId, next->eventId);
    if (!ev || !ev->persistent)
        return false;

    DungeonEventProgress const& prog =
        context->GetValue<DungeonEventProgress&>("dungeon clear event progress")->Get();
    // stepIndex >= 1 means the event has advanced past its first step, so this is
    // false until the tank has actually arrived and the event has begun running.
    return prog.eventId == ev->id && prog.stepIndex >= 1 &&
           prog.stepIndex < ev->steps.size();
}
