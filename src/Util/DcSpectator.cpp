/*
 * mod-dungeon-clear — DcSpectator.cpp
 *
 * See DcSpectator.h for the design. The load-bearing detail: the possess
 * branch of Unit::SetCharmedBy sets UNIT_FLAG_DISABLE_MOVE on the CHARMER
 * (the player body). Nearly every MotionMaster entry point refuses a
 * UNIT_FLAG_DISABLE_MOVE owner — including the two DungeonClear uses,
 * MovePoint (MotionMaster.cpp:488) and MoveSplinePath (MotionMaster.cpp:506) —
 * so without clearing that flag the bot body freezes solid the moment the
 * camera detaches. We clear it immediately after a successful possess; it is
 * a one-time set (nothing re-asserts it during possession), and
 * RemoveCharmedBy clears it again on teardown, so the early clear is harmless
 * on exit.
 */

#include "Util/DcSpectator.h"

#include <unordered_map>

#include "Chat.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "TemporarySummon.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

namespace
{
    // player guid -> possessed camera-dummy guid. Never store the dummy as a
    // raw Creature* — resolve via ObjectAccessor at each use so a despawn race
    // can't dangle. World-thread only, so no locking.
    std::unordered_map<ObjectGuid, ObjectGuid> g_spectators;

    void Announce(Player* player, char const* msg)
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(msg);
    }

    bool Refuse(std::string* whyNot, char const* reason)
    {
        if (whyNot)
            *whyNot = reason;
        return false;
    }
}

namespace DcSpectator
{
    bool IsActive(Player* player)
    {
        return player && g_spectators.count(player->GetGUID()) != 0;
    }

    bool Start(Player* player, std::string* whyNot)
    {
        if (!player)
            return Refuse(whyNot, "No player.");
        if (IsActive(player))
            return Refuse(whyNot, "Already spectating.");
        if (!player->IsAlive())
            return Refuse(whyNot, "You are dead.");
        if (player->GetVehicle())
            return Refuse(whyNot, "Cannot spectate while on a vehicle.");
        if (player->IsInFlight())
            return Refuse(whyNot, "Cannot spectate while on a flight path.");
        if (player->GetCharmGUID())
            return Refuse(whyNot, "Cannot spectate while controlling another unit.");

        // Core-defined invisible utility template — no creature_template SQL
        // row needed, runtime hardening below covers the rest.
        TempSummon* dummy = player->SummonCreature(WORLD_TRIGGER,
            player->GetPosition(), TEMPSUMMON_MANUAL_DESPAWN);
        if (!dummy)
            return Refuse(whyNot, "Failed to summon the camera.");

        dummy->SetFaction(FACTION_FRIENDLY);
        dummy->SetImmuneToAll(true);
        dummy->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
        dummy->SetReactState(REACT_PASSIVE);

        if (!dummy->SetCharmedBy(player, CHARM_TYPE_POSSESS))
        {
            dummy->DespawnOrUnsummon();
            return Refuse(whyNot, "Possession failed.");
        }

        // Mandatory: the possess branch just set UNIT_FLAG_DISABLE_MOVE on the
        // player body, which MotionMaster::MovePoint (MotionMaster.cpp:488) and
        // MoveSplinePath (MotionMaster.cpp:506) refuse — the bot AI could no
        // longer move the body. Clear it so the body keeps clearing.
        player->RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE);

        // AFTER possession the dummy is client-controlled, so SetCanFly routes
        // SMSG_MOVE_SET_CAN_FLY to the controlling session with the proper
        // order counter. (Pre-possession the packet path differs — keep order.)
        float const speed = DcSettings::GetFloat(player, "SpectateSpeed");
        dummy->SetCanFly(true);
        dummy->SetSpeed(MOVE_FLIGHT, speed, true);
        dummy->SetSpeed(MOVE_RUN, speed, true);

        g_spectators[player->GetGUID()] = dummy->GetGUID();
        Announce(player, "Spectator camera on — your character stays under bot control. Toggle again to return.");
        return true;
    }

    void Stop(Player* player)
    {
        if (!player)
            return;

        auto it = g_spectators.find(player->GetGUID());
        if (it == g_spectators.end())
            return;

        ObjectGuid const dummyGuid = it->second;
        g_spectators.erase(it);

        if (Creature* dummy = ObjectAccessor::GetCreature(*player, dummyGuid))
        {
            dummy->RemoveCharmedBy(player);
            dummy->DespawnOrUnsummon();
        }
        else if (player->GetCharmGUID() == dummyGuid)
        {
            // Stale half-state: the dummy is gone but the charm bookkeeping
            // still points at it. StopCastingCharm walks the generic uncharm
            // path and restores client control.
            player->StopCastingCharm();
        }

        // Deliberately no motion-master surgery on the body here: DC's own AI
        // re-issues movement on its next tick. Per the stale-MoveFollow lesson,
        // only clean up what WE installed — the possession and the dummy.

        Announce(player, "Spectator camera off.");
    }

    bool Toggle(Player* player, std::string* whyNot)
    {
        if (IsActive(player))
        {
            Stop(player);
            return true;
        }
        return Start(player, whyNot);
    }
}
