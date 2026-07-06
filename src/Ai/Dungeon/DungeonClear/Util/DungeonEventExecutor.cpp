/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonEventExecutor.h"

#include <cmath>
#include <optional>
#include <unordered_set>

#include "Creature.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GossipDef.h"
#include "Group.h"
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
    // How far out a Gossip step ACQUIRES its (unique) NPC in order to walk to it.
    // Deliberately wide: the freed crew can settle well beyond the gossip range
    // (the ZulFarrak crew descend to the temple floor), and the approach must
    // start from wherever the prior steps left the tank — so this is the "walk to
    // the NPC" radius, distinct from the 5yd interaction range. The entry is
    // unique, so a wide flat scan can only return the intended NPC.
    constexpr float DC_EVENT_GOSSIP_APPROACH = 100.0f;

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

    // A Jump step bridges an OFF-MESH gap (a drop-down ledge): the leader leaps
    // it, but the followers' path-follow has no navmesh across the gap and they
    // strand on the lip. Once the leader has landed, relocate any party BOT still
    // stuck on the far side to the tank. (A follower jump would hit the same
    // missing mesh; the teleport is the robust fix and is user-sanctioned.)
    void PullStrandedFollowersAcross(Player* leader, float lx, float ly, float lz)
    {
        Group* group = leader->GetGroup();
        if (!group)
            return;

        // Past this from the landing => "didn't make the drop". Comfortably wider
        // than the jump span so a follower that DID land is never yanked back.
        constexpr float DC_JUMP_STRANDED_DIST = 15.0f;

        uint32 idx = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == leader)
                continue;
            if (!member->IsInWorld() || !member->IsAlive())
                continue;
            if (member->GetMapId() != leader->GetMapId())
                continue;
            if (!GET_PLAYERBOT_AI(member))  // only relocate bots, never a human
                continue;
            if (member->GetExactDist(lx, ly, lz) <= DC_JUMP_STRANDED_DIST)
                continue;                   // already across

            // Land them ON the landing, fanned out a little so they don't stack on
            // one point. Fan around the explicit (lx,ly,lz) rather than the leader's
            // live position: a TeleportParty leader is relocated by a NearTeleportTo
            // whose position update can lag a tick (pending ack), so reading its live
            // coords could scatter the followers back at the checkpoint. (For Jump /
            // DropInHole the leader is already physically on the landing, so this is
            // the same point.) Drop any stale follow spline first so it can't drag
            // them back toward the lip.
            float const angle = leader->GetOrientation() + static_cast<float>(idx) * 0.7f;
            float const off = 1.5f + 0.5f * static_cast<float>(idx);
            float const tx = lx + std::cos(angle) * off;
            float const ty = ly + std::sin(angle) * off;

            member->GetMotionMaster()->Clear();
            member->NearTeleportTo(tx, ty, lz, member->GetOrientation(),
                                   /*casting*/ false, /*vehicle*/ false, /*withPet*/ true);
            ++idx;

            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] {} pulled stranded follower {} across the jump gap",
                      leader->GetName(), member->GetName());
        }
    }
}

bool DungeonEventExecutor::IsOnDropLanding(Player* bot, EventStep const& step)
{
    if (!bot)
        return false;
    MotionMaster* mm = bot->GetMotionMaster();
    // The MoveFall spline runs in an EFFECT_MOTION_TYPE generator that POPS the
    // moment the spline reaches the floor — a reliable, server-managed "the fall
    // FINISHED" signal, so we fire only once the bot is actually at the bottom (not
    // mid-shaft, which teleported the party into the walls). Do NOT also test
    // MOVEMENTFLAG_FALLING: a server bot has no client to send the fall-land packet
    // that clears it, so it stays stuck on and would wedge this gate forever (the
    // party strands up top and the tank dies — the first observed bug). Require the
    // bot to also be well below the hole mouth (step.z, the ledge height) so a tick
    // before the fall has started (no EFFECT generator yet) doesn't read as landed.
    bool const fallSplineRunning =
        mm && mm->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE;
    return !fallSplineRunning && bot->GetPositionZ() < step.z - 15.0f;
}

bool DungeonEventExecutor::SelectGossip(Player* bot, Creature* npc, int32 option)
{
    if (!bot || !npc)
        return false;

    // Open the menu — synchronously populates PlayerTalkClass with this NPC as
    // the gossip-menu sender and its DB options.
    bot->SetFacingToObject(npc);
    WorldPacket hello;
    hello << npc->GetGUID();
    bot->GetSession()->HandleGossipHelloOpcode(hello);

    GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
    if (menu.GetMenuItems().empty() ||
        !menu.GetItem(static_cast<uint32>(option)))
        return false;  // menu/option not ready yet — caller retries

    // Send the NPC's OWN guid: HandleGossipSelectOptionOpcode rejects the select
    // unless the packet guid equals the open menu's sender GUID, and a real bot's
    // master isn't targeting this NPC. See the Gossip step note below.
    WorldPacket select;
    select << npc->GetGUID() << menu.GetMenuId()
           << static_cast<uint32>(option);
    select << std::string();  // no coded box
    bot->GetSession()->HandleGossipSelectOptionOpcode(select);
    return true;
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

        case EventStepKind::Jump:
        {
            float const radius = step.radius > 0.0f ? step.radius : DC_EVENT_MOVE_RADIUS;
            if (bot->GetExactDist(step.x, step.y, step.z) <= radius)
            {
                // Landed. The followers can't path across the off-mesh gap, so
                // pull any stranded on the far side over to the tank. Idempotent:
                // a follower already across is skipped, so a restart is a no-op.
                PullStrandedFollowersAcross(bot, step.x, step.y, step.z);
                return StepResult::Done;
            }
            // While the jump spline is in flight the bot reads as moving — don't
            // re-issue (MoveJump would restart the arc and never land). Only fire
            // a fresh jump once the bot is settled on the lip.
            if (!bot->isMoving())
            {
                float const speed = bot->GetSpeed(MOVE_RUN);
                MotionMaster* mm = bot->GetMotionMaster();
                mm->Clear();
                mm->MoveJump(step.x, step.y, step.z, speed, speed, 1);
                LOG_DEBUG("playerbots.dungeonclear",
                          "[dungeon-clear] {} event-step Jump -> ({:.1f},{:.1f},{:.1f})",
                          bot->GetName(), step.x, step.y, step.z);
            }
            return StepResult::Running;
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
            // Diagnostic: a ClearRadius that completes in ~0ms is "premature" —
            // it found no hostile because the tank evaluated it from too far
            // (arriveRadius too large for the design assumption that 'arrived'
            // means 'among the mobs'). Log the gap so that's unambiguous: bot's
            // distance to the clear centre + whether a target was found.
            if (!u)
            {
                float const botToCentre =
                    bot->GetExactDist(step.x, step.y, step.z);
                LOG_DEBUG("playerbots.dungeonclear",
                          "[DC:{}] ClearRadius DONE: no hostile in r={:.0f} of "
                          "({:.0f},{:.0f},{:.0f}); botDistToCentre={:.1f}",
                          bot->GetName(), r, step.x, step.y, step.z, botToCentre);
            }
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
            // Acquire across a WIDE approach radius and walk to the NPC, instead
            // of gating the approach on a tight search radius. The preceding engage
            // steps often complete instantly — the ZulFarrak temple bosses are
            // already dead from the continuous wave combat — so they never drive
            // the tank's descent and leave it parked up at the ramp, while Weegli /
            // Bly walk down to the temple floor (>40yd). A tight acquire radius then
            // strands this step until timeout (observed: the Weegli gossip never
            // fired and the door to Chief Ukorz never opened). An author-specified
            // larger radius is still honoured.
            float const search = step.radius > DC_EVENT_GOSSIP_APPROACH
                                     ? step.radius
                                     : DC_EVENT_GOSSIP_APPROACH;
            Creature* npc = bot->FindNearestCreature(step.creatureEntry, search, /*alive*/ true);
            if (!npc)
            {
                // Optional target: if it's not merely outside the approach radius
                // but actually gone (no alive one anywhere nearby), SKIP the step
                // rather than waiting forever for an NPC that will never come (a
                // freed ZulFarrak helper the party let die). The wide rescan
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

            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] {} event-step Gossip {} '{}' option {}",
                      bot->GetName(), npc->GetGUID().ToString(), npc->GetName(),
                      step.gossipOption);

            // Drive the open-menu + select opcodes (shared with the escort step's
            // self-heal start). Running while the menu/option isn't populated yet.
            return SelectGossip(bot, npc, step.gossipOption) ? StepResult::Done
                                                             : StepResult::Running;
        }

        case EventStepKind::EscortCreature:
        {
            // PURE COMPLETION GATE only. The follow + threat-engage + self-heal
            // gossip + watchdog are all driven by the ACTION
            // (DcObjectiveArriveAction::DriveEscortCreature), which owns the tick
            // while the escort is in progress and only falls through to this Drive
            // once the final boss exists. Done strictly when that boss is up (grid
            // scan) or its encounter bit is set — NEVER on "reached the end" (the
            // DM-West / RFD premature-completion class of bug).
            if (step.escortDoneEntry &&
                bot->FindNearestCreature(step.escortDoneEntry, DC_EVENT_CREATURE_SCAN,
                                         /*alive*/ true))
                return StepResult::Done;
            if (step.escortDoneBit >= 0)
            {
                InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
                if (inst && (inst->GetCompletedEncounterMask() &
                             (1u << static_cast<uint32>(step.escortDoneBit))))
                    return StepResult::Done;
            }
            return StepResult::Running;
        }

        case EventStepKind::DropInHole:
        {
            // PURE gate + follower teleport. The leader's glide-over-the-hole and
            // pure-vertical MoveFall are driven by the ACTION (DriveDropInHole),
            // which owns the tick so the at-objective Hold can't cancel the off-mesh
            // nudge spline; RunStep is reached only once the action hands back — i.e.
            // the leader has finished falling and is down on the deep floor. Pull any
            // follower still held up top down to the landing (they can't reproduce
            // the off-mesh nudge; the teleport is the sanctioned one-way-drop fix),
            // then report Done so the objective latches and the clear advances to the
            // escort. Idempotent: a follower already down is skipped.
            if (!DungeonEventExecutor::IsOnDropLanding(bot, step))
                return StepResult::Running;
            // Clear the stuck fall state. MoveFall set MOVEMENTFLAG_FALLING and no
            // client ever clears it for a bot, so without this the leader stays
            // "falling" forever and can't swim/walk out to the escort afterward.
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
            bot->GetMotionMaster()->Clear();
            // MoveFall targets the vmap GROUND (the lakebed), which in WC sits a few
            // yards below the water-SURFACE navmesh sheet the mmaps bake (the deep
            // floor we routed to). Settle the leader exactly on the landing so stock
            // nav resumes cleanly (and the followers fan out around it there).
            if (std::fabs(bot->GetPositionZ() - step.landZ) > 3.0f)
                bot->NearTeleportTo(step.landX, step.landY, step.landZ,
                                    bot->GetOrientation());
            PullStrandedFollowersAcross(bot, step.landX, step.landY, step.landZ);
            return StepResult::Done;
        }

        case EventStepKind::TeleportParty:
        {
            // User-sanctioned one-way relocation across a navmesh break the bots
            // cannot path (a big DIAGONAL drop: a pure-vertical DropInHole would land
            // in the wrong column and a ballistic Jump clips/overshoots). The
            // objective anchor's navigation walks the leader up the ramp to the
            // checkpoint; once there, teleport the leader down to the landing and pull
            // every party bot across to it. Fully synchronous — no action driver, no
            // mid-fall tick ownership — so it returns Done the same tick it fires.
            float const radius = step.radius > 0.0f ? step.radius : DC_EVENT_MOVE_RADIUS;
            // Idempotent: already on the landing (a tick-gap restart before the
            // objective latched) -> re-pull any straggler and report Done, never
            // re-teleport the leader.
            if (bot->GetExactDist(step.landX, step.landY, step.landZ) <= radius)
            {
                PullStrandedFollowersAcross(bot, step.landX, step.landY, step.landZ);
                return StepResult::Done;
            }
            // The at-objective Hold keeps the leader on the checkpoint; with a
            // generous gate radius the objective's own arrival always satisfies this,
            // so the teleport never fires from mid-ramp. (Reached only if combat
            // somehow displaced the leader.) Actively re-approach the checkpoint —
            // mirroring the MoveTo step — instead of passively waiting on a hold
            // nothing drives: without this a fight at the checkpoint that drags the
            // leader >radius idles the (required) event until its timeout fails it
            // into a stall. By construction the checkpoint is connected mesh the
            // leader stood on seconds ago.
            if (bot->GetExactDist(step.x, step.y, step.z) > radius)
            {
                HopTo(bot, step.x, step.y, step.z);
                return StepResult::Running;
            }
            bot->GetMotionMaster()->Clear();
            bot->NearTeleportTo(step.landX, step.landY, step.landZ, bot->GetOrientation());
            PullStrandedFollowersAcross(bot, step.landX, step.landY, step.landZ);
            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] {} TeleportParty: ({:.1f},{:.1f},{:.1f}) -> "
                      "landing ({:.1f},{:.1f},{:.1f})",
                      bot->GetName(), step.x, step.y, step.z,
                      step.landX, step.landY, step.landZ);
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
        // The EscortCreature step has NO flat timeout: a 30s default would mis-fire
        // during the Disciple's 32.5s banish channel and the long ritual hold. Its
        // own dead-air watchdog (DriveEscortCreature) owns liveness instead, so the
        // step is never escalated to Failed here regardless of elapsed time.
        bool const watchdogOwned = step.kind == EventStepKind::EscortCreature;
        // Escalate a step that has run too long to Failed. uint32 subtraction is
        // wrap-safe for an elapsed interval.
        if (!watchdogOwned && timeout > 0 &&
            static_cast<uint32>(nowMs - prog.stepStartMs) >= timeout)
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

void DungeonEventExecutor::SweepCompletedConditionalEvents(Player* bot,
                                                           AiObjectContext* context,
                                                           uint32 mapId)
{
    if (!bot || !context)
        return;

    std::vector<DungeonEvent const*> const conditional = DungeonEventRegistry::Conditional(mapId);
    if (conditional.empty())
        return;

    auto& cleared =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear cleared anchors")->Get();
    auto& seenDue =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear seen due events")->Get();

    for (DungeonEvent const* ev : conditional)
    {
        // A room-aggro pre-clear repeats per boss (its "room trash remains"
        // condition toggles each pull), so a momentary clear is NOT completion —
        // its folded note tracks the gated boss's death instead (DcBossesAction).
        // Genuinely repeatable events likewise never reach a terminal "done".
        if (ev->repeatable || DungeonEventRegistry::IsRoomAggroPreClear(*ev))
            continue;

        uint32 const lk = ConditionalLatchKey(ev->id);
        if (cleared.count(lk))
            continue;

        if (EventConditionRegistry::Evaluate(ev->conditionId, bot, context))
        {
            // Currently due: remember we saw it active so a later transition to
            // not-due reads as completion rather than not-yet-started.
            seenDue.insert(lk);
        }
        else if (seenDue.count(lk))
        {
            // Was due, now isn't, and the executor never latched it: its gating
            // condition WAS the latch (e.g. a Stratholme ziggurat whose instance
            // data flips 1 -> 2 the instant the Ash'ari Crystal topples, mid-
            // combat, before the dormant executor can run its completion tick).
            // Latch it so the folded panel note flips to (done) and — because the
            // boss signature counts cleared.size() — the boss list re-pushes.
            cleared.insert(lk);
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] conditional event '{}' (id {}) completed via condition "
                      "transition -> latched done", bot->GetName(), ev->name, ev->id);
        }
    }
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

    // The progress must belong to the CURRENT instance. Drive() resets a prior
    // run's progress on an instance change — but that reset runs only AFTER this
    // trigger gate has fired, so on the first tick of a new run the leftover
    // progress (stepIndex from the previous instance) is still present here. Were
    // this sticky to fire off that stale stepIndex, the at-objective trigger would
    // activate with the tank nowhere near the anchor, Drive would then advance the
    // event's first step (a KillCreature gate false-completes when its creature is
    // merely out of the 250yd scan range, i.e. the tank is far away), and the run
    // would pin on a half-started event whose later steps can never reach their GO
    // — a permanent "Blocked". Tying the latch to the progress's stamped instance
    // (set by Drive only once the tank has genuinely arrived and driven a step)
    // keeps it false until the event has actually begun in THIS instance.
    Unit* const self = context->GetValue<Unit*>("self target")->Get();
    uint32 const instanceId = self ? self->GetInstanceId() : 0;
    if (instanceId == 0 || prog.instanceId != instanceId)
        return false;

    // stepIndex >= 1 means the event has advanced past its first step, so this is
    // false until the tank has actually arrived and the event has begun running.
    return prog.eventId == ev->id && prog.stepIndex >= 1 &&
           prog.stepIndex < ev->steps.size();
}
