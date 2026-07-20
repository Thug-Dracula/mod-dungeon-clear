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
#include "MotionMaster.h"
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
    constexpr uint32 DM_GO_DOOR_OPEN      = 2;     // GO_STATE_ACTIVE_ALTERNATIVE

    ObjectiveArriveResult FireDefiasCannon(Player* bot, AiObjectContext* /*context*/,
                                           DungeonBossInfo const& /*info*/)
    {
        // Door already open -> the cannon has fired.
        if (GameObject* door = bot->FindNearestGameObject(DM_GO_IRON_CLAD_DOOR, 100.0f))
            if (door->GetGoState() != GO_STATE_READY)
                return ObjectiveArriveResult::Done;

        GameObject* cannon = bot->FindNearestGameObject(DM_GO_CANNON, 40.0f);
        if (!cannon)
            return ObjectiveArriveResult::Running;  // not in range yet

        // Directly open the door. The CastItemUseSpell path (grant gunpowder
        // → use item on cannon → SmartAI spellhit → door opens) is unreliable
        // because the 2s cast can be interrupted by movement or aggro, and
        // the bot often gets attacked by nearby patrols while standing at the
        // cannon. Opening the door directly bypasses the entire item/spell
        // chain and is functionally identical — the party passes through to
        // the ship either way.
        if (GameObject* door = bot->FindNearestGameObject(DM_GO_IRON_CLAD_DOOR, 100.0f))
        {
            door->SetGoState(GO_STATE_ACTIVE_ALTERNATIVE);
            door->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE);
        }

        return ObjectiveArriveResult::Done;
    }

    // --- Old Hillsbrad: GrantIncendiaryBombs (hook id 3) ------------------
    // Brazen (18725) only offers his drake ride to Durnholde Keep when the player
    // HOLDS the Pack of Incendiary Bombs (item 25853) — gossip menu 7959 option 0
    // is condition-gated on that item. A player gets the pack by talking to Erozion
    // (18723), but Erozion's grant option is quest-gated (quest 10283 "Taretha's
    // Diversion" taken/rewarded), which a bot never has — so the bot can never
    // select Erozion's gossip. Grant the pack directly (mechanically identical to
    // Erozion's AddItem) so Brazen's ride option then appears. Idempotent: Done
    // once the tank holds it. (The same pack is used to bomb the barrels in
    // objective 2 — the UseItemOnGO step also self-grants as a backstop.)
    constexpr uint32 OH_ITEM_INCENDIARY_BOMBS = 25853;

    ObjectiveArriveResult GrantIncendiaryBombs(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        if (bot->GetItemByEntry(OH_ITEM_INCENDIARY_BOMBS))
            return ObjectiveArriveResult::Done;
        bot->AddItem(OH_ITEM_INCENDIARY_BOMBS, 1);
        return bot->GetItemByEntry(OH_ITEM_INCENDIARY_BOMBS)
                   ? ObjectiveArriveResult::Done
                   : ObjectiveArriveResult::Running;  // bags full this tick — retry
    }

    // --- The Mechanar: GrantCacheKeyAndLoot (hook id 4) -------------------
    // The Cache of the Legion (GO 184465 normal / 184849 heroic) is a locked chest
    // (Lock.dbc 1706, a LOCK_KEY_ITEM lock) requiring the Cache of the Legion Key
    // (item 30438). Blizzard's key is formed by combining the two Jagged Crystals
    // the Gatewatchers drop (Gyro-Kill -> Blue 30436, Iron-Hand -> Red 30437) via
    // the crystals' on-use spell 36565 — but a bot never right-clicks a crystal,
    // and in a party the two crystals can land in different bags. Per the design
    // decision (grant the key), hand the leader key 30438 directly, then OPEN the
    // chest exactly the way the Deadmines cannon / Old Hillsbrad barrels do it:
    // USE the key item on the GO (CastItemUseSpell). This is load-bearing — the
    // original "grant key + let the stock loot pipeline open it" plan DEADLOCKED
    // (the tank parked at the cache and never looted). Two independent stock
    // failings sink that plan: (a) MaybeSkipUnworthyLoot blacklists every chest
    // with DungeonClear.IgnoreChests on (the default), and (b) even reached,
    // OpenLootAction's CanOpenLock has the LOCK_KEY_ITEM case COMMENTED OUT, so it
    // can never produce an opening spell for a key-item lock — and the core only
    // opens such a lock when the KEY ITEM ITSELF is the cast item (Spell::
    // CanOpenLock: m_CastItem->GetEntry() == lockInfo->Index[j]). So the leader
    // must use the key on the chest itself; the resulting Player::SendLoot fires
    // SMSG_LOOT_RESPONSE -> the always-on "store loot" handler auto-stores it.
    // The chest is consumable (leaves GO_READY once used), so the loot-state check
    // is the completion latch; idempotent + self-healing across an event restart.
    constexpr uint32 MECH_GO_CACHE_NORMAL = 184465;
    constexpr uint32 MECH_GO_CACHE_HEROIC = 184849;
    constexpr uint32 MECH_ITEM_CACHE_KEY  = 30438;  // on-use spell 3366 (OPEN_LOCK)
    constexpr float  MECH_CACHE_SEARCH    = 25.0f;
    // Cast reach: the OPEN_LOCK ends in Player::SendLoot, which range-checks the
    // GO's interact box (the Deadmines/Durnholde item-use plants measured failing
    // past ~6yd). Stay well inside it; the objective anchor sits ON the chest, but
    // arrival jitter can leave the tank a couple of yards out, so walk the final
    // gap in rather than spam-casting from range.
    constexpr float  MECH_CACHE_CAST_REACH = 4.0f;

    ObjectiveArriveResult GrantCacheKeyAndLoot(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        GameObject* cache = bot->FindNearestGameObject(MECH_GO_CACHE_NORMAL, MECH_CACHE_SEARCH);
        if (!cache)
            cache = bot->FindNearestGameObject(MECH_GO_CACHE_HEROIC, MECH_CACHE_SEARCH);

        if (!cache)
        {
            // No cache in range: either the tank has not arrived / scanned it yet,
            // or it has already been looted and despawned (consumable). Once we
            // have handed over the key the latter is true -> Done; otherwise keep
            // waiting to arrive.
            return bot->HasItemCount(MECH_ITEM_CACHE_KEY, 1)
                       ? ObjectiveArriveResult::Done
                       : ObjectiveArriveResult::Running;
        }

        // The chest leaves GO_READY the instant the key-use OPEN_LOCK lands (the
        // loot window is up and the "store loot" handler is draining it) — a
        // stable, idempotent "this cache is done" latch.
        if (cache->getLootState() != GO_READY)
            return ObjectiveArriveResult::Done;

        // The 2s key "Opening" cast is already running — let it complete rather
        // than re-issue and interrupt ourselves every tick.
        if (bot->IsNonMeleeSpellCast(false))
            return ObjectiveArriveResult::Running;

        // Grant the key (bots never combined the crystals).
        Item* key = bot->GetItemByEntry(MECH_ITEM_CACHE_KEY);
        if (!key)
        {
            bot->AddItem(MECH_ITEM_CACHE_KEY, 1);
            key = bot->GetItemByEntry(MECH_ITEM_CACHE_KEY);
            if (!key)
                return ObjectiveArriveResult::Running;  // bags full this tick — retry
        }

        // Close the final yards if the objective anchor left us just outside the
        // interact box (SendLoot's range check is unforgiving). Nothing else moves
        // the tank while the event holds, so this walk-in sticks.
        if (bot->GetExactDist(cache) > MECH_CACHE_CAST_REACH)
        {
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, cache->GetPositionX(), cache->GetPositionY(),
                                                  cache->GetPositionZ());
            return ObjectiveArriveResult::Running;
        }

        // USE the key ON the chest: item-use supplies m_CastItem so the KEY_ITEM
        // lock opens -> SendLoot -> SMSG_LOOT_RESPONSE -> "store loot" stores it.
        SpellCastTargets targets;
        targets.SetGOTarget(cache);
        bot->CastItemUseSpell(key, targets, 0, 0);
        return ObjectiveArriveResult::Running;  // the loot-state latch above confirms it
    }

    // --- Zul'Farrak Witch Doctor Zum'rah: TriggerZumrahAreatrigger (hook id 5) -
    // Zum'rah (7271) starts neutral (faction 35) and only becomes hostile when
    // the party crosses areatrigger 962 near his cauldron. Bots' movement may
    // skip the areatrigger check, so we fire it explicitly when the tank arrives.
    constexpr uint32 ZF_ZUMRAH_TRIGGER = 962;

    ObjectiveArriveResult TriggerZumrahAreatrigger(Player* bot, AiObjectContext* /*context*/,
                                                    DungeonBossInfo const& /*info*/)
    {
        // Check if Zum'rah is already hostile (faction 37 = Sandfury trolls).
        // If so, the trigger already fired — no-op.
        Creature* zumrah = bot->FindNearestCreature(7271, 30.0f);
        if (!zumrah || zumrah->GetFaction() == 37)
            return ObjectiveArriveResult::Done;

        WorldPacket p(CMSG_AREATRIGGER);
        p << uint32(ZF_ZUMRAH_TRIGGER);
        p.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(p);
        return ObjectiveArriveResult::Done;
    }

    // hookId -> behaviour. To give an objective on-arrival behaviour, add a row
    // here and reference its id from a BossRosterRegistry objective (onArriveHook)
    // or a Custom event step (DungeonEventRegistry).
    std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const& Hooks()
    {
        static std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const kHooks = {
            { 1, &EnsureRingStarted },       // BRD Ring of Law — start the arena event
            { 2, &FireDefiasCannon },        // Deadmines — fire the cannon, open the door
            { 3, &GrantIncendiaryBombs },    // Old Hillsbrad — pack of bombs (unlocks Brazen)
            { 4, &GrantCacheKeyAndLoot },    // The Mechanar — Cache of the Legion key + loot
            { 5, &TriggerZumrahAreatrigger },// Zul'Farrak — fire areatrigger 962 for Zum'rah
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
