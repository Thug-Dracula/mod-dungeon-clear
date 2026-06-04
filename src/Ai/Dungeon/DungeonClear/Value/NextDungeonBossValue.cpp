/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "NextDungeonBossValue.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "Player.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearStateValues.h"
#include "Playerbots.h"

namespace
{
    // A boss "exists alive" on the current map if any spawned creature with the
    // matching entry is alive. Returns:
    //   present=true,  alive=true   — at least one live instance
    //   present=true,  alive=false  — instance(s) exist but all are dead
    //   present=false, alive=false  — no spawned instance found at all
    struct BossLiveState
    {
        bool present;
        bool alive;
    };

    // One pass over the creature store builds the liveness of every boss
    // entry of interest, instead of re-scanning the whole store once per boss
    // (the old per-boss CheckBossLive was O(bosses x store)). `wanted` is the
    // set of boss entries we care about; entries never seen stay absent and
    // read as {present=false, alive=false} via the caller's default.
    std::unordered_map<uint32, BossLiveState> BuildLiveness(Map* map,
                                                            std::unordered_set<uint32> const& wanted)
    {
        std::unordered_map<uint32, BossLiveState> liveness;
        if (!map || wanted.empty())
            return liveness;

        for (auto const& kv : map->GetCreatureBySpawnIdStore())
        {
            Creature* c = kv.second;
            if (!c)
                continue;
            uint32 const entry = c->GetEntry();
            if (!wanted.count(entry))
                continue;
            BossLiveState& state = liveness[entry];  // default {false,false}
            state.present = true;
            if (c->IsAlive())
                state.alive = true;
        }
        return liveness;
    }

    BossLiveState LookupLive(std::unordered_map<uint32, BossLiveState> const& liveness, uint32 entry)
    {
        auto it = liveness.find(entry);
        return it != liveness.end() ? it->second : BossLiveState{false, false};
    }

    // From a same-tier candidate list (all currently fightable, or all
    // not-yet-spawned), pick the boss to head toward. `stickyEntry` is the boss
    // chosen on the previous computation.
    //
    // COMMIT-AND-HOLD. Once we've committed to a boss we return it unchanged
    // for as long as it remains in this candidate tier — no per-tick re-ranking
    // by distance, no per-tick reachability re-probe. Both of those signals are
    // noisy near a decision boundary: straight-line distance swings as the bot
    // rounds corners, and the bounded 2-stride IsReachable probe flickers when
    // the bot is wedged. Re-deciding on them every recompute makes the target
    // (and the long-path rebuilt from it) flip-flop between two bosses, which
    // wedges the bot between competing routes. The commit is released only when
    // the boss leaves `cands` entirely — killed (mask bit set), skipped, or
    // despawned — all handled by the caller's partitioning, so a gone boss
    // simply isn't here to match.
    //
    // The initial pick (no valid commit) walks the candidates in DBC encounter
    // order — which is the order they arrive in here, since the caller builds
    // `cands` by iterating the encounter-index-sorted "dungeon bosses" list —
    // and takes the first one. This advances strictly to the next boss in
    // numerical order rather than to whichever boss happens to be physically
    // nearest: when boss N dies and the commit releases, the bot heads to N+1,
    // not to a closer-but-later N+2.
    //
    // Do NOT re-rank by straight-line distance here. Distance ordering is what
    // made the target jump ahead multiple bosses — the route to the next boss
    // often loops away from it, so a later boss reads as nearer. It also
    // interacts badly with the reachability probe below: IsReachable is a
    // bounded 2-stride check that reads false for any boss more than a couple
    // strides off (i.e. every remaining boss right after a kill), so the
    // fallback path runs almost every fresh pick — and a distance-sorted
    // fallback front() is precisely the nearest boss, reintroducing the jump.
    //
    // Reachability is kept only as a forward-looking preference: among the
    // in-order candidates, take the first the bounded probe confirms reachable,
    // so a lower-index boss walled off behind an unopened event doesn't wedge
    // the bot when a reachable boss sits right after it. When the probe is
    // unsure about all of them (the common post-kill case) we fall back to the
    // lowest-index boss. A boss that turns out to be unreachable after commit
    // is handled by Advance's stall/skip path, not by silently re-targeting.
    std::optional<DungeonBossInfo> PickTarget(Player* bot,
                                              std::vector<DungeonBossInfo>& cands,
                                              uint32 stickyEntry)
    {
        if (cands.empty())
            return std::nullopt;

        // Hold the commit if the boss is still a candidate in this tier.
        if (stickyEntry)
            for (DungeonBossInfo const& info : cands)
                if (info.entry == stickyEntry)
                    return info;

        // Fresh selection, in encounter-index order: first reachable, else the
        // lowest-index boss overall.
        for (DungeonBossInfo const& info : cands)
            if (DungeonClearUtil::IsReachable(bot, info.x, info.y, info.z))
                return info;

        return cands.front();  // lowest index; reachability unconfirmed
    }
}

std::optional<DungeonBossInfo> NextDungeonBossValue::Calculate()
{
    if (!bot || !bot->IsInWorld())
        return std::nullopt;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return std::nullopt;

    std::vector<DungeonBossInfo> const& bosses =
        AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");

    std::unordered_set<uint32> const& skipped =
        AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped");

    // The completed-encounter mask is the authoritative kill signal. It is
    // set by Map::UpdateEncounterState from the same KillRewarder path that
    // grants loot and quest credit (KillRewarder.cpp -> UpdateEncounterState),
    // flipping bit (1 << DungeonEncounter.dbc encounterIndex). BossSpawnIndex
    // stores that exact DBC encounterIndex, so the bit lines up directly.
    //
    // Do NOT use InstanceScript::GetBossState(encounterIndex) here: that
    // indexes the script's internal bosses[] vector by its own DATA_*
    // constants — a different index space that only aligns with the DBC
    // encounterIndex by coincidence. When it doesn't align, a misaligned slot
    // reading DONE silently skips a live boss, and a slot that never reads
    // DONE leaves a freshly-killed boss looking alive (the symptom: party
    // kills a boss but never advances to the next one). The mask is reliable
    // for every dungeon whose encounters are ENCOUNTER_CREDIT_KILL_CREATURE,
    // which is precisely the set BossSpawnIndex indexes.
    InstanceScript* inst = DungeonClearUtil::GetInstanceScript(bot);
    uint32 const completedMask = inst ? inst->GetCompletedEncounterMask() : 0u;

    // Resolve every boss's liveness in a single store pass (shared by the
    // selected-override check and the partition loop below).
    std::unordered_set<uint32> wantedEntries;
    wantedEntries.reserve(bosses.size());
    for (DungeonBossInfo const& info : bosses)
        wantedEntries.insert(info.entry);
    std::unordered_map<uint32, BossLiveState> const liveness = BuildLiveness(map, wantedEntries);

    // Check if there is a manually selected boss target override
    uint32 const selectedEntry = AI_VALUE(uint32, "dungeon clear selected boss");
    if (selectedEntry != 0)
    {
        for (DungeonBossInfo const& info : bosses)
        {
            if (info.entry == selectedEntry)
            {
                bool invalid = false;
                if (info.encounterIndex < 32 && (completedMask & (1u << info.encounterIndex)))
                    invalid = true;

                if (!invalid)
                {
                    BossLiveState const state = LookupLive(liveness, info.entry);
                    if (state.present && !state.alive)
                        invalid = true;
                }

                if (invalid)
                {
                    // Already dead/completed: clear override and drop back to automatic
                    context->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
                    break;
                }

                // Force return this boss and update sticky
                context->GetValue<uint32>("dungeon clear sticky boss")->Set(info.entry);
                return info;
            }
        }
    }

    // Partition the uncleared bosses by liveness instead of returning the
    // first one in DBC encounter order. `alive` bosses can be fought right
    // now; `missing` bosses have no spawn on the map yet (far grid not loaded,
    // or a boss that only spawns after an event). We never engage a corpse —
    // a present-but-dead boss whose completion bit hasn't flipped yet is
    // transient and is skipped, matching the prior fall-through behavior and
    // keeping the empty-result == all-cleared contract that AllClearedTrigger
    // relies on.
    std::vector<DungeonBossInfo> alive;
    std::vector<DungeonBossInfo> missing;
    for (DungeonBossInfo const& info : bosses)
    {
        if (skipped.count(info.entry))
            continue;

        // Authoritative: this encounter is already complete in this instance.
        if (info.encounterIndex < 32 && (completedMask & (1u << info.encounterIndex)))
            continue;

        BossLiveState const state = LookupLive(liveness, info.entry);
        if (state.alive)
            alive.push_back(info);
        else if (!state.present)
            missing.push_back(info);
    }

    // Prefer a boss we can fight now over one that hasn't spawned: this stops
    // the bot from locking onto an early-indexed unspawned boss (and stalling
    // with "not spawned, use dc skip" in Advance) while a later-indexed boss
    // is alive and killable. Within each tier we commit to the previously
    // chosen boss until it leaves the candidate set (see PickTarget).
    uint32 const stickyEntry = AI_VALUE(uint32, "dungeon clear sticky boss");

    std::optional<DungeonBossInfo> pick = PickTarget(bot, alive, stickyEntry);
    if (!pick)
        pick = PickTarget(bot, missing, stickyEntry);

    // Record the commit so the next computation holds it. Storing 0 on an empty
    // result releases the commit cleanly when the dungeon is cleared or every
    // boss is skipped.
    context->GetValue<uint32>("dungeon clear sticky boss")->Set(pick ? pick->entry : 0u);
    return pick;
}
