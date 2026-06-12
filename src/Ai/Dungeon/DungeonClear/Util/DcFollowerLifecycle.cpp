/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcFollowerLifecycle.h"

#include "DungeonClearUtil.h"   // DC_PULL_* log macros

#include "DungeonClearMath.h"
#include "DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "AttackersValue.h"
#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "InstanceScript.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Chat.h"
#include "ServerFacade.h"
#include "Timer.h"
#include "World.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"

namespace
{
    // GUIDs of players that currently carry a DC follow-tank MoveFollow
    // generator. Mutated from the follow-tank action (bot AI update) and read by
    // the reaper (world update / OnPlayerbotUpdate); both run on the world
    // thread today, but the set is tiny and the lock is uncontended, so guard it
    // anyway to stay correct if bot updates ever move off-thread.
    std::set<ObjectGuid> g_dcFollowingPlayers;
    std::mutex g_dcFollowingMutex;

    // GUIDs of followers DC has put into the mod-playerbots "passive" combat
    // strategy for an advanced pull. Tracked so ReapStrandedPassives is the
    // single authoritative teardown — it removes passive the moment the leader
    // leaves a holding pull phase, even for a follower that got dragged into
    // combat (its own engines can't self-heal a passive-lock mid-fight). Only
    // passive DC itself applied lives here; a player's manual passive is never
    // recorded, so it's never clobbered.
    std::set<ObjectGuid> g_dcPassivePlayers;
    std::mutex g_dcPassiveMutex;

    // Followers are released from passive on a short delay AFTER the leader
    // commits the pull (flips to Engage), so the tank gets a head start on
    // threat before DPS pile on. Maps a follower GUID to the getMSTime()
    // deadline at which it drops passive. Only the graceful Engage transition is
    // delayed; a hard exit (run ended / paused / leader gone) and the
    // camp-safety valve release immediately. Guarded by g_dcPassiveMutex.
    std::map<ObjectGuid, uint32> g_dcPlayerReleaseAt;

    // Pets are released from passive on a further delay AFTER their owner: a pet
    // freed the instant its owner drops passive charges in and bodies the pack
    // before the tank has settled aggro, botching the pull. Maps a follower GUID
    // to the getMSTime() deadline at which its pet flips back to REACT_DEFENSIVE.
    // Guarded by g_dcPassiveMutex (same lifecycle as g_dcPassivePlayers).
    std::map<ObjectGuid, uint32> g_dcPetReleaseAt;

    // Healers held at camp are pinned with the "stay" strategy (not "+passive" —
    // they must keep casting heals) so playerbots' reach-to-heal MOVEMENT
    // (ReachTargetAction early-outs on HasStrategy("stay")) is suppressed and the
    // healer tops the tank off IN PLACE instead of running forward to chase heal
    // range. Maps a healer GUID to the bitmask of states DC added the strategy in
    // (bit0 = BOT_STATE_COMBAT, bit1 = BOT_STATE_NON_COMBAT) so release strips
    // exactly what we applied and leaves a player's own manual "stay" alone.
    // Guarded by g_dcPassiveMutex (same lifecycle as g_dcPassivePlayers).
    std::map<ObjectGuid, uint8> g_dcHealerStayStates;
}

// Flip any pets whose post-owner-release grace window has elapsed back to
// REACT_DEFENSIVE. Kept separate from the player sweep below because pet
// releases outlive g_dcPassivePlayers: the owner is dropped from that set the
// instant it's freed, but its pet stays scheduled here for a few more seconds.
static void ReapPetReleases()
{
    std::vector<ObjectGuid> due;
    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        if (g_dcPetReleaseAt.empty())
            return;
        uint32 const now = getMSTime();
        for (auto it = g_dcPetReleaseAt.begin(); it != g_dcPetReleaseAt.end();)
        {
            // getMSTime() wraps ~every 49 days; the signed-diff test stays
            // correct across the wrap where (now >= deadline) would not.
            if (int32(now - it->second) >= 0)
            {
                due.push_back(it->first);
                it = g_dcPetReleaseAt.erase(it);
            }
            else
                ++it;
        }
    }

    for (ObjectGuid guid : due)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld())
            continue;
        if (Pet* pet = player->GetPet())
            pet->SetReactState(REACT_DEFENSIVE);
    }
}

void DcFollowerLifecycle::MarkFollowing(ObjectGuid player)
{
    std::lock_guard<std::mutex> lock(g_dcFollowingMutex);
    g_dcFollowingPlayers.insert(player);
}
void DcFollowerLifecycle::UnmarkFollowing(ObjectGuid player)
{
    std::lock_guard<std::mutex> lock(g_dcFollowingMutex);
    g_dcFollowingPlayers.erase(player);
}
void DcFollowerLifecycle::ReapOrphanedFollows()
{
    std::lock_guard<std::mutex> lock(g_dcFollowingMutex);
    if (g_dcFollowingPlayers.empty())
        return;

    for (auto it = g_dcFollowingPlayers.begin(); it != g_dcFollowingPlayers.end();)
    {
        Player* player = ObjectAccessor::FindPlayer(*it);

        // Player left the world (logged out, bot despawned): nothing to clear,
        // and the GUID would otherwise linger forever. Drop it.
        if (!player || !player->IsInWorld())
        {
            it = g_dcFollowingPlayers.erase(it);
            continue;
        }

        // AI still ticking -> its own follow-tank teardown owns this generator;
        // leave the mark in place and move on.
        if (GET_PLAYERBOT_AI(player))
        {
            ++it;
            continue;
        }

        // AI gone but the player is still in world (a self-bot toggled out of bot
        // mode). Cancel the leftover continuous follow so movement control reverts
        // to the human; a real player has no AI to self-heal it otherwise.
        if (player->GetMotionMaster() &&
            player->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            if (player->isMoving())
                player->StopMoving();
            player->GetMotionMaster()->Clear();
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] reaped orphaned follow generator (self-bot left bot "
                     "mode) -> movement control returned to player",
                     player->GetName());
        }

        it = g_dcFollowingPlayers.erase(it);
    }
}
void DcFollowerLifecycle::EnsureTankBearForm(Player* bot)
{
    if (!bot || bot->getClass() != CLASS_DRUID)
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    // Already shifted into a bear form — nothing to do.
    if (botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr))
        return;
    // Prefer dire bear form (more armor + the bear/dire-bear HP bonus); the
    // dire-bear action's alternatives fall back to plain bear form, but invoke
    // it explicitly too so an untrained dire bear still shifts.
    if (!botAI->DoSpecificAction("dire bear form", Event(), true))
        botAI->DoSpecificAction("bear form", Event(), true);
}
void DcFollowerLifecycle::ApplyFollowerPassive(Player* follower)
{
    if (!follower)
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(follower);
    if (!botAI)
        return;

    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        // Already DC-managed — nothing to do (idempotent across hold ticks).
        if (g_dcPassivePlayers.count(follower->GetGUID()))
            return;
    }

    // Healers are deliberately NOT made fully passive: they must keep casting
    // heals on the tank through the drag-back. But they ARE pinned with the
    // "stay" strategy so they heal IN PLACE. The bug this fixes: a healer chasing
    // heal range runs playerbots' reach-to-heal movement (ReachPartyMemberToHeal
    // -> MovementAction::ReachCombatTo), which, when the heal target is moving
    // FORWARD and the healer is approaching from BEHIND it, predicts the target's
    // position by pushing the destination up to 3yd PAST the target along its
    // facing. On a pull the tank faces the pack and is running, so the healer's
    // destination lands beyond the tank, INTO the mobs — it ran past the tank and
    // aggroed more. "stay" disables exactly that action (ReachTargetAction::
    // isUseful early-outs on HasStrategy("stay")) while leaving the cast-heal
    // action free, so the healer only heals when the tank is already in range/LOS
    // and otherwise waits at camp. We join g_dcPassivePlayers so
    // ReapStrandedPassives releases the healer (clearing the stay pin) on the SAME
    // schedule as the held DPS — it only advances once they do. Any aggro a heal
    // still pulls is the tank's to taunt off; the pet stays defensive (untouched).
    if (PlayerbotAI::IsHeal(follower))
    {
        uint8 added = 0;
        if (!botAI->HasStrategy("stay", BOT_STATE_COMBAT))
        {
            botAI->ChangeStrategy("+stay", BOT_STATE_COMBAT);
            added |= 0x1;
        }
        if (!botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        {
            botAI->ChangeStrategy("+stay", BOT_STATE_NON_COMBAT);
            added |= 0x2;
        }
        {
            std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
            g_dcPassivePlayers.insert(follower->GetGUID());
            g_dcHealerStayStates[follower->GetGUID()] = added;
            // Re-holding within the post-release grace window: cancel any pending
            // pet release so a defensive pet isn't flipped unexpectedly.
            g_dcPetReleaseAt.erase(follower->GetGUID());
        }
        DC_PULL_DEBUG("[DC:{}] advanced-pull: healer pinned at camp (stay, heals only)",
                      follower->GetName());
        return;
    }

    // A passive the player set themselves is left entirely alone: don't add a
    // duplicate, and don't record it (so we never strip it on release).
    if (botAI->HasStrategy("passive", BOT_STATE_COMBAT))
        return;

    botAI->ChangeStrategy("+passive", BOT_STATE_COMBAT);
    if (Pet* pet = follower->GetPet())
        pet->SetReactState(REACT_PASSIVE);

    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        g_dcPassivePlayers.insert(follower->GetGUID());
        // Re-holding within the post-release grace window: cancel the pending
        // pet release so the pet we just set passive isn't flipped back.
        g_dcPetReleaseAt.erase(follower->GetGUID());
    }

    DC_PULL_DEBUG("[DC:{}] advanced-pull: held passive at camp", follower->GetName());
}
void DcFollowerLifecycle::RemoveFollowerPassive(Player* follower)
{
    if (!follower)
        return;

    uint8 healerStay = 0;
    bool isHealerStay = false;
    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        // Only strip passive we applied; a manual passive was never recorded.
        if (!g_dcPassivePlayers.erase(follower->GetGUID()))
            return;
        // The release is happening now — drop any armed graceful-release timer.
        g_dcPlayerReleaseAt.erase(follower->GetGUID());
        // A held healer carries the "stay" pin instead of "+passive": remember
        // which states WE added so we strip exactly those (and leave a player's
        // own manual stay alone).
        auto it = g_dcHealerStayStates.find(follower->GetGUID());
        if (it != g_dcHealerStayStates.end())
        {
            isHealerStay = true;
            healerStay = it->second;
            g_dcHealerStayStates.erase(it);
        }
    }

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(follower))
    {
        // Healer release: drop the movement pin so it can rejoin the party, then
        // fall through (a held healer was never made passive, so the passive
        // strip below is a harmless no-op and the pet was never touched).
        if (isHealerStay)
        {
            if ((healerStay & 0x1) && botAI->HasStrategy("stay", BOT_STATE_COMBAT))
                botAI->ChangeStrategy("-stay", BOT_STATE_COMBAT);
            if ((healerStay & 0x2) && botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("-stay", BOT_STATE_NON_COMBAT);
            DC_PULL_DEBUG("[DC:{}] advanced-pull: healer released (stay cleared)",
                          follower->GetName());
            return;
        }

        if (botAI->HasStrategy("passive", BOT_STATE_COMBAT))
            botAI->ChangeStrategy("-passive", BOT_STATE_COMBAT);

        // Hold the pet passive a little longer than its owner: releasing them in
        // lockstep lets the pet bolt in and pull aggro off the tank before he's
        // settled. Schedule the flip back to REACT_DEFENSIVE; ReapPetReleases
        // (ticked from ReapStrandedPassives) applies it once the delay elapses.
        // A non-positive delay reverts to the old immediate release.
        float const delaySec = DcSettings::GetFloat(follower, "PullPetReleaseDelay");
        if (delaySec > 0.0f && follower->GetPet())
        {
            std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
            g_dcPetReleaseAt[follower->GetGUID()] =
                getMSTime() + uint32(delaySec * 1000.0f);
        }
        else if (Pet* pet = follower->GetPet())
            pet->SetReactState(REACT_DEFENSIVE);

        DC_PULL_DEBUG("[DC:{}] advanced-pull: released from passive", follower->GetName());
    }
}
void DcFollowerLifecycle::ReapStrandedPassives()
{
    // Apply any due pet releases first: these persist after the owner has been
    // dropped from g_dcPassivePlayers, so they must run even when the player
    // sweep below early-returns on an empty set.
    ReapPetReleases();

    std::vector<ObjectGuid> marked;
    {
        std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
        if (g_dcPassivePlayers.empty())
            return;
        marked.assign(g_dcPassivePlayers.begin(), g_dcPassivePlayers.end());
    }

    float const safetyHp = sConfigMgr->GetOption<float>("DungeonClear.PullSafetyHpPct", 50.0f);

    for (ObjectGuid guid : marked)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);

        // Player gone, or its AI was deleted (self-bot left bot mode): we can no
        // longer drive a ChangeStrategy, so just drop the mark. The strategy was
        // never persisted to the DB (direct ChangeStrategy doesn't Save), so it
        // evaporates with the engine anyway.
        if (!player || !player->IsInWorld() || !GET_PLAYERBOT_AI(player))
        {
            std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
            g_dcPassivePlayers.erase(guid);
            g_dcPlayerReleaseAt.erase(guid);
            g_dcHealerStayStates.erase(guid);
            continue;
        }

        uint32 phase = 0;
        Position camp;
        bool const inPull = DcLeaderSignal::GetLeaderPullInfo(player, phase, camp);

        // Release this follower the moment the leader leaves a holding phase.
        // GetLeaderPullInfo returns true for Engage too (only Idle returns
        // false), so a plain !inPull test would keep the party passive through
        // the entire camp fight: the tank flips to Engage on arrival but only
        // drops to Idle out of combat — which never happens while it's tanking.
        // Gate on IsPullPhaseHolding instead so Engage releases the party (per
        // the DcPullPhase contract: "Idle and Engage release them"), along with
        // pull off / dc off / paused / leader gone. The single authoritative
        // teardown — fires regardless of the follower's own engine state.
        if (!inPull || !DcLeaderSignal::IsPullPhaseHolding(phase))
        {
            // The graceful pull commit (leader reached camp and flipped to
            // Engage, so inPull is still true) can hold the party passive a
            // little longer to give the tank a threat head start before DPS
            // pile on — the player-side analogue of PullPetReleaseDelay, and the
            // clock the pet delay then stacks on. A hard exit (run ended,
            // paused, dc off, leader gone -> !inPull) always releases at once,
            // as does a zero delay.
            float const delaySec =
                inPull ? DcSettings::GetFloat(player, "PullPlayerReleaseDelay") : 0.0f;
            if (delaySec <= 0.0f)
            {
                RemoveFollowerPassive(player);
                continue;
            }

            uint32 const now = getMSTime();
            bool releaseNow = false;
            {
                std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
                auto it = g_dcPlayerReleaseAt.find(guid);
                if (it == g_dcPlayerReleaseAt.end())
                    // First Engage tick for this follower — arm the timer.
                    g_dcPlayerReleaseAt[guid] = now + uint32(delaySec * 1000.0f);
                else if (int32(now - it->second) >= 0)
                    releaseNow = true;
            }
            if (releaseNow)
                RemoveFollowerPassive(player);
            continue;
        }

        // Still in a holding phase. If a graceful-release timer was armed (a
        // brief Engage flicker, or the leader re-entered a hold), disarm it so
        // the party isn't dropped mid-hold.
        {
            std::lock_guard<std::mutex> lock(g_dcPassiveMutex);
            g_dcPlayerReleaseAt.erase(guid);
        }

        // Camp-safety valve: a held, passive follower taking real damage (a
        // patrol clipped camp, or the pull went sideways) can't defend itself —
        // abort the pull so the whole party drops passive and fights back. This
        // is an emergency, so it bypasses the graceful release delay entirely.
        if (player->IsInCombat() && safetyHp > 0.0f &&
            player->GetHealthPct() < safetyHp)
        {
            DC_PULL_INFO("[DC:{}] advanced-pull SAFETY: passive follower at {:.0f}% in "
                         "combat -> aborting pull, releasing party",
                         player->GetName(), player->GetHealthPct());
            DcLeaderSignal::AbortLeaderPull(player);
            RemoveFollowerPassive(player);
        }
    }
}
