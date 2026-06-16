/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <unordered_map>

#include "GameObject.h"
#include "InstanceScript.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

namespace
{
    // --- Blackrock Depths Ring of Law: EnsureRingStarted (hook id 1) -------
    // The Ring of Law starts when a party member crosses area trigger 1526 at
    // the arena centre. Normally that's the human (or a self-bot relayed from
    // the master). But a bot never autonomously sends CMSG_AREATRIGGER, so an
    // all-bot party (or a human not on the trigger when the tank arrives) would
    // never start it. Used as the Ring of Law event's second step (a Custom
    // step), this fires the REAL trigger from the leader once it's standing on
    // the spot: the core validates the bot is within the trigger radius and runs
    // at_ring_of_law for real (which itself honours the 2-minute post-wipe
    // cooldown and no-ops if already started). Done once the encounter is at
    // least IN_PROGRESS; Running (and re-fires, harmlessly idempotent) until then.
    constexpr uint32 BRD_TYPE_RING_OF_LAW = 1;  // DataTypes::TYPE_RING_OF_LAW
    constexpr uint32 BRD_RING_IN_PROGRESS = 1;  // EncounterState::IN_PROGRESS
    constexpr uint32 BRD_RING_OF_LAW_TRIGGER = 1526;

    ObjectiveArriveResult EnsureRingStarted(Player* bot, AiObjectContext* /*context*/,
                                            DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // Already running (or DONE) — let the hold step take over.
        if (inst->GetData(BRD_TYPE_RING_OF_LAW) >= BRD_RING_IN_PROGRESS)
            return ObjectiveArriveResult::Done;

        // Fire the real area trigger from the leader. Same supported path
        // ReachAreaTriggerAction uses for a non-teleport trigger; the core
        // range-checks and runs the at_ring_of_law script.
        WorldPacket p(CMSG_AREATRIGGER);
        p << uint32(BRD_RING_OF_LAW_TRIGGER);
        p.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(p);
        return ObjectiveArriveResult::Running;
    }

    // --- Deadmines Defias Cannon: FireDefiasCannon (hook id 2) ------------
    // The way to the pirate ship is sealed by the Iron Clad Door (GO 16397,
    // lock 202 — rogue-pick only, so a bot party can never click it open). A
    // player loots the Defias Gunpowder chest, then uses the gunpowder on the
    // Defias Cannon (GO 16398): that casts spell 6250 at the cannon, whose
    // SmartGameObjectAI (SMART_EVENT_SPELLHIT on 6250) fires the cannon —
    // opening the door to GO_STATE_ACTIVE_ALTERNATIVE (the GOState enum's
    // "closed door open by cannon fire"), summoning two Defias Squallshapers,
    // and setting the cannon instance data.
    //
    // A bot never loots the chest or uses the item, so we replay the authentic
    // spell hit directly: CastSpell at the cannon GO explicitly targets it
    // (Unit::CastSpell(GameObject*)), so it lands in the spell's GO-target list
    // and the core delivers go->AI()->SpellHit — running the real action list.
    // No duplication of the door/summon logic.
    //
    // Used as the Deadmines event's Custom step (DeadminesEvents). Idempotent:
    // if the door is already open the cannon has fired, so we never cast twice
    // (a second hit would summon two more Squallshapers). Returns Running after
    // firing; the event's door-state gate is what confirms the open and advances.
    constexpr uint32 DM_GO_CANNON         = 16398;
    constexpr uint32 DM_GO_IRON_CLAD_DOOR = 16397;
    constexpr uint32 DM_SPELL_GUNPOWDER   = 6250;  // item 5397's use-spell

    ObjectiveArriveResult FireDefiasCannon(Player* bot, AiObjectContext* /*context*/,
                                           DungeonBossInfo const& /*info*/)
    {
        // The door already open (state != READY) means the cannon has fired —
        // never fire a second time (covers a re-init after an earlier attempt
        // left the instance's door open).
        if (GameObject* door = bot->FindNearestGameObject(DM_GO_IRON_CLAD_DOOR, 100.0f))
            if (door->GetGoState() != GO_STATE_READY)
                return ObjectiveArriveResult::Done;

        GameObject* cannon = bot->FindNearestGameObject(DM_GO_CANNON, 40.0f);
        if (!cannon)
            return ObjectiveArriveResult::Running;  // not in range yet

        bot->CastSpell(cannon, DM_SPELL_GUNPOWDER, true);  // -> SmartAI spell-hit
        return ObjectiveArriveResult::Running;  // door-state gate confirms it
    }

    // hookId -> behaviour. To give an objective on-arrival behaviour, add a row
    // here and reference its id from a BossRosterRegistry objective (onArriveHook)
    // or a Custom event step (DungeonEventRegistry).
    std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const& Hooks()
    {
        static std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const kHooks = {
            { 1, &EnsureRingStarted },  // BRD Ring of Law — start the arena event
            { 2, &FireDefiasCannon },   // Deadmines — fire the cannon, open the door
        };
        return kHooks;
    }
}

ObjectiveArriveResult ObjectiveHookRegistry::Run(uint32 hookId, Player* bot, AiObjectContext* context,
                                                 DungeonBossInfo const& info)
{
    if (hookId == 0)
        return ObjectiveArriveResult::Done;

    auto const& hooks = Hooks();
    auto it = hooks.find(hookId);
    if (it == hooks.end() || !it->second)
        return ObjectiveArriveResult::Done;

    return it->second(bot, context, info);
}
