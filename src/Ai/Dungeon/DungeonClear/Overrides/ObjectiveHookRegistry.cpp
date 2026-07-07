/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <atomic>
#include <unordered_map>

#include "GameObject.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "Spell.h"
#include "Timer.h"
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
    // player loots the Defias Gunpowder chest, then USES the gunpowder on the
    // Defias Cannon (GO 16398): that casts spell 6250 at the cannon, whose
    // SmartGameObjectAI (SMART_EVENT_SPELLHIT on 6250) fires the cannon —
    // opening the door to GO_STATE_ACTIVE_ALTERNATIVE (the GOState enum's
    // "closed door open by cannon fire"), summoning two Defias Squallshapers,
    // and setting the cannon instance data.
    //
    // Spell 6250 is SPELL_EFFECT_OPEN_LOCK against the cannon, and the cannon's
    // lock (83) is an ITEM lock requiring the Defias Gunpowder (item 5397) as
    // the key — so the spell only HITS the cannon (and fires the SmartAI) when
    // it is cast FROM that item, supplying the cast-item the lock check needs.
    // A bare CastSpell(6250) with no cast item fails the lock and never hits.
    //
    // A bot never loots the chest, so grant it the gunpowder and use the item on
    // the cannon via the full item-use path. Used as the Deadmines event's
    // Custom step (DeadminesEvents). The gunpowder has a 2s "Opening" cast, so:
    //  - if the door is already open, the cannon has fired -> Done (idempotent;
    //    also covers a re-init after an earlier attempt left the door open, where
    //    a second fire would summon two more Squallshapers);
    //  - if the opening cast is already in flight, let it finish (don't re-cast
    //    and interrupt ourselves every tick);
    //  - otherwise grant + use the gunpowder. Returns Running; the event's
    //    door-state gate is what confirms the open and advances.
    constexpr uint32 DM_GO_CANNON         = 16398;
    constexpr uint32 DM_GO_IRON_CLAD_DOOR = 16397;
    constexpr uint32 DM_ITEM_GUNPOWDER    = 5397;  // casts OPEN_LOCK spell 6250

    ObjectiveArriveResult FireDefiasCannon(Player* bot, AiObjectContext* /*context*/,
                                           DungeonBossInfo const& /*info*/)
    {
        // Door already open (state != READY) -> the cannon has fired.
        if (GameObject* door = bot->FindNearestGameObject(DM_GO_IRON_CLAD_DOOR, 100.0f))
            if (door->GetGoState() != GO_STATE_READY)
                return ObjectiveArriveResult::Done;

        // The 2s gunpowder "Opening" cast is already running — let it complete.
        if (bot->IsNonMeleeSpellCast(false))
            return ObjectiveArriveResult::Running;

        GameObject* cannon = bot->FindNearestGameObject(DM_GO_CANNON, 40.0f);
        if (!cannon)
            return ObjectiveArriveResult::Running;  // not in range yet

        // Grant the gunpowder (bots never looted the chest) and use it on the
        // cannon: the item is the OPEN_LOCK key, so the spell hits and the
        // cannon's SmartAI fires.
        Item* gunpowder = bot->GetItemByEntry(DM_ITEM_GUNPOWDER);
        if (!gunpowder)
        {
            bot->AddItem(DM_ITEM_GUNPOWDER, 1);
            gunpowder = bot->GetItemByEntry(DM_ITEM_GUNPOWDER);
            if (!gunpowder)
                return ObjectiveArriveResult::Running;  // bags full this tick
        }

        SpellCastTargets targets;
        targets.SetGOTarget(cannon);
        bot->CastItemUseSpell(gunpowder, targets, 0, 0);
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
    {
        // A non-zero hookId that resolves to nothing is a wiring bug (a typo, or a
        // hook that was removed). Latching Done would silently pass a step/objective
        // authored to do real work; Blocked surfaces it (the run stalls for the
        // human) and the throttled warn names the id. Registry-integrity gtest
        // catches this at author time; this is the runtime backstop.
        static std::atomic<uint32> lastWarnMs{0};
        uint32 const now = getMSTime();
        uint32 prev = lastWarnMs.load(std::memory_order_relaxed);
        if (now - prev > 10000u &&
            lastWarnMs.compare_exchange_strong(prev, now, std::memory_order_relaxed))
        {
            LOG_WARN("playerbots", "DungeonClear: objective hookId {} is not registered "
                                   "(broken wiring) -> Blocked", hookId);
        }
        return ObjectiveArriveResult::Blocked;
    }

    return it->second(bot, context, info);
}

bool ObjectiveHookRegistry::Has(uint32 hookId)
{
    if (hookId == 0)
        return false;
    auto const& hooks = Hooks();
    auto it = hooks.find(hookId);
    return it != hooks.end() && it->second;
}
