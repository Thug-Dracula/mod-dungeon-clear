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

    // From a same-tier candidate list (all in encounter-index order), pick the
    // first boss the bounded reachability probe confirms reachable, else the
    // lowest-index one. A lower-index boss walled off behind an unopened event
    // shouldn't wedge the bot when a reachable boss sits right after it; when
    // the probe is unsure about all of them (the common post-kill case) we fall
    // back to the lowest-index boss.
    //
    // Do NOT re-rank by straight-line distance: the route to the next boss
    // often loops away from it, so a later boss reads as nearer and the target
    // jumps ahead multiple bosses.
    std::optional<DungeonBossInfo> FirstReachableOrFront(
        Player* bot, std::vector<DungeonBossInfo> const& cands)
    {
        if (cands.empty())
            return std::nullopt;
        for (DungeonBossInfo const& info : cands)
            if (DungeonClearUtil::IsReachable(bot, info.x, info.y, info.z))
                return info;
        return cands.front();  // lowest index; reachability unconfirmed
    }

    // From a same-tier candidate list (all currently fightable, or all
    // not-yet-spawned), pick the boss to head toward. `stickyEntry` is the boss
    // chosen on the previous computation; `stickyEncounterIndex` is its DBC
    // encounter index when `haveStickyIndex` (the sticky boss is still known in
    // the "dungeon bosses" list, even if it's now dead and absent from `cands`).
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
    // ADVANCE-FORWARD. When the commit releases (the boss we were heading to is
    // gone), we head to the next boss *after* it in encounter order, not to the
    // lowest-index survivor. Snapping to the lowest index is correct only when
    // the party clears strictly from boss #1; a party that started mid-list
    // (e.g. via a manual `dungeon clear boss #3` selection, or by skipping
    // early bosses) would otherwise walk back to boss #1 on every kill. We take
    // the candidates with encounter index strictly greater than the boss we
    // just left and pick within them; only if nothing remains ahead do we wrap
    // to the full set to mop up any lower-index bosses left behind.
    //
    // A boss that turns out to be unreachable after commit is handled by
    // Advance's stall/skip path, not by silently re-targeting.
    std::optional<DungeonBossInfo> PickTarget(Player* bot,
                                              std::vector<DungeonBossInfo>& cands,
                                              uint32 stickyEntry,
                                              uint32 stickyEncounterIndex,
                                              bool haveStickyIndex)
    {
        if (cands.empty())
            return std::nullopt;

        // Hold the commit if the boss is still a candidate in this tier.
        if (stickyEntry)
            for (DungeonBossInfo const& info : cands)
                if (info.entry == stickyEntry)
                    return info;

        // Commit released: prefer the next boss after the one we just left.
        if (haveStickyIndex)
        {
            std::vector<DungeonBossInfo> ahead;
            ahead.reserve(cands.size());
            for (DungeonBossInfo const& info : cands)
                if (info.encounterIndex > stickyEncounterIndex)
                    ahead.push_back(info);
            if (std::optional<DungeonBossInfo> pick = FirstReachableOrFront(bot, ahead))
                return pick;
        }

        // Fresh selection (no prior commit) or wrap-around (nothing ahead).
        return FirstReachableOrFront(bot, cands);
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

    // Resolve the committed boss's encounter index from the full boss list (it
    // stays here even after the boss dies and drops out of the candidate
    // tiers), so PickTarget can advance to the next boss after it rather than
    // snapping back to the lowest-index survivor when the commit releases.
    uint32 stickyEncounterIndex = 0;
    bool haveStickyIndex = false;
    if (stickyEntry)
        for (DungeonBossInfo const& info : bosses)
            if (info.entry == stickyEntry)
            {
                stickyEncounterIndex = info.encounterIndex;
                haveStickyIndex = true;
                break;
            }

    std::optional<DungeonBossInfo> pick =
        PickTarget(bot, alive, stickyEntry, stickyEncounterIndex, haveStickyIndex);
    if (!pick)
        pick = PickTarget(bot, missing, stickyEntry, stickyEncounterIndex, haveStickyIndex);

    // Record the commit so the next computation holds it. Storing 0 on an empty
    // result releases the commit cleanly when the dungeon is cleared or every
    // boss is skipped.
    context->GetValue<uint32>("dungeon clear sticky boss")->Set(pick ? pick->entry : 0u);
    return pick;
}
