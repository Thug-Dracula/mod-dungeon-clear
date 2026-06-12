/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearRoomTrashValue.h"

#include <optional>

#include "AttackersValue.h"
#include "Creature.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"

GuidVector DungeonClearRoomTrashValue::Calculate()
{
    GuidVector out;

    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return out;

    // Feature master switch. Off -> the room-clear gate/driver never engages and
    // the boss is pulled exactly as before.
    if (!DcSettings::GetBool(bot, "ClearRoomBeforeBoss"))
        return out;

    // Only meaningful during an active, unpaused run.
    if (!AI_VALUE(bool, "dungeon clear enabled") || AI_VALUE(bool, "dungeon clear paused"))
    {
        trackedBoss = 0;
        gaveUp = false;
        noteSent = false;
        return out;
    }

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!next.has_value())
    {
        trackedBoss = 0;
        gaveUp = false;
        noteSent = false;
        return out;
    }

    RoomAggroBoss const* room =
        RoomAggroRegistry::Find(bot->GetMapId(), next->entry);
    if (!room)
    {
        trackedBoss = 0;
        gaveUp = false;
        noteSent = false;
        return out;
    }

    // Boss change resets the no-progress timeout and the give-up latch so each
    // room starts with a clean clock (self-heals across bosses/runs/skips).
    if (trackedBoss != room->bossEntry)
    {
        trackedBoss = room->bossEntry;
        lastRemaining = 0;
        lastProgressMs = getMSTime();
        gaveUp = false;
        noteSent = false;
    }

    // Already gave up on this room -> report it clear so the boss gate opens and
    // the run proceeds (the straggler comes with the boss). Stays latched until
    // the boss changes.
    if (gaveUp)
        return out;

    // Measure from the LIVE boss (some flagged bosses wander) — the room is its
    // radius-sphere, not the static spawn anchor.
    Creature* liveBoss = DcTargeting::GetLiveBoss(bot, context, room->bossEntry);
    if (!liveBoss)
        return out;  // not loaded/alive yet — nothing to measure or clear

    // Units inside the boss's own aggro sphere come with the boss and are
    // excluded (IsRoomTrash). Sized from the LIVE boss's real aggro range, PLUS
    // both combat reaches and the run's aggro margin: a melee tank that walks up
    // to the nearest still-clearable unit (just outside this sphere) then stands
    // within a combat reach of it, so the reaches keep the tank itself outside
    // the boss's aggro even at point-blank on that unit — the whole point of the
    // exclusion is "don't wake the boss while clearing".
    float const bossSafe = liveBoss->GetAggroRange(bot) +
                           bot->GetCombatReach() + liveBoss->GetCombatReach() +
                           DcSettings::GetFloat(bot, "AggroRangeMargin");

    GuidVector const& candidates = AI_VALUE(GuidVector, "dungeon clear far targets");
    for (ObjectGuid const guid : candidates)
    {
        Unit* u = ObjectAccessor::GetUnit(*bot, guid);
        if (!u || !u->IsAlive())
            continue;
        if (u->GetGUID() == liveBoss->GetGUID())
            continue;
        if (!bot->IsHostileTo(u))
            continue;
        if (!AttackersValue::IsPossibleTarget(u, bot))
            continue;
        // Never treat another encounter boss as room trash — the dedicated
        // at-boss path owns bosses, and scripted bosses misbehave when pulled.
        if (DcTargeting::IsDungeonBossEntry(context, u->GetEntry()))
            continue;
        // ...nor a RoomAggroRegistry boss entry. Some encounter partners are
        // swapped OUT of the boss roster (SM Cathedral's High Inquisitor Whitemane
        // -> Mograine, inheriting the kill-bit) so IsDungeonBossEntry no longer
        // flags them — but they are still bosses that share THIS room pull and
        // stand WITH the live boss. Chasing one as "trash" drags the tank into the
        // boss's aggro sphere (the live SM Cathedral failure: the tank orbited
        // Mograine trying to reach Whitemane, who stands inside his aggro, and woke
        // the room). The registry lists both partners, so skip any of them.
        if (RoomAggroRegistry::Find(bot->GetMapId(), u->GetEntry()))
            continue;

        float const distToBoss = liveBoss->GetExactDist(u);
        if (!RoomAggroRegistry::IsRoomTrash(*room, u->GetEntry(), distToBoss, bossSafe))
            continue;

        // Don't count a unit the tank can't actually reach (different floor with
        // no route) or one behind a closed door — clearing it isn't possible
        // without help, and the no-progress timeout would otherwise livelock.
        if (!DcEngageGeometry::IsLevelReachable(bot, u))
            continue;
        if (DcEngageGeometry::ClosedDoorBetween(bot, u->GetPositionX(),
                                                u->GetPositionY(), u->GetPositionZ()))
            continue;

        out.push_back(guid);
    }

    // No-progress give-up valve: if the remaining count hasn't DROPPED within
    // RoomClearTimeout seconds, stop holding the boss pull on an unreachable
    // straggler / a respawn churn and let the run proceed. Progress (a smaller
    // set, or first sighting) re-arms the clock.
    uint32 const now = getMSTime();
    uint32 const remaining = static_cast<uint32>(out.size());
    if (remaining == 0)
    {
        lastRemaining = 0;
        lastProgressMs = now;
        return out;
    }

    // CRITICAL: the room-clear driver (DungeonClearRoomTrashTrigger) only fires
    // once the tank is at the boss's engage range — while it's still walking the
    // long path to the room, no trash is being engaged and the count can't drop.
    // So keep the clock RE-ARMED until the tank actually arrives; otherwise the
    // whole timeout is burned by the travel leg and the room-clear "gives up"
    // before the first pull (a 192yd approach trivially outlasts 30s). The
    // no-progress window must measure stalls WHILE CLEARING, not the walk in.
    bool const atBoss =
        DcEngageGeometry::IsAtBossEngage(bot, context, *next, DC_ENGAGE_RANGE);
    if (!atBoss)
    {
        lastRemaining = remaining;
        lastProgressMs = now;
        return out;
    }

    if (lastRemaining == 0 || remaining < lastRemaining)
    {
        lastRemaining = remaining;
        lastProgressMs = now;
    }

    uint32 const timeoutMs = DcSettings::GetUInt(bot, "RoomClearTimeout") * 1000u;
    if (timeoutMs && (now - lastProgressMs) > timeoutMs)
    {
        gaveUp = true;
        if (!noteSent && DcLeaderSignal::IsDungeonClearLeader(bot))
        {
            noteSent = true;
            DcStatusPublisher::SendAddonMessage(
                botAI, "CHAT\tCouldn't fully clear the room before " + next->name +
                           " — pulling anyway (some adds will join).");
        }
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] room-clear gave up on {} ({} left after {}s) -> pulling boss",
                 bot->GetName(), next->name, remaining,
                 DcSettings::GetUInt(bot, "RoomClearTimeout"));
        out.clear();
    }

    return out;
}
