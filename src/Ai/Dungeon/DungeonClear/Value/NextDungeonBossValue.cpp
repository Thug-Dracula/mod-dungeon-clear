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

    // From the encounter-ordered candidate list, pick the boss to head toward.
    // `stickyEntry` is the boss chosen on the previous computation;
    // `stickyEncounterIndex` is its DBC encounter index when `haveStickyIndex`
    // (the sticky boss is still known in the "dungeon bosses" list, even if it's
    // now dead and absent from `cands`).
    //
    // COMMIT-AND-HOLD. Once we've committed to a boss we return it unchanged
    // for as long as it remains in the candidate list — no per-tick re-ranking
    // by distance, no per-tick reachability re-probe. Both of those signals are
    // noisy near a decision boundary: straight-line distance swings as the bot
    // rounds corners, and the bounded 2-stride IsReachable probe flickers when
    // the bot is wedged. Re-deciding on them every recompute makes the target
    // (and the long-path rebuilt from it) flip-flop between two bosses, which
    // wedges the bot between competing routes. The commit is released only when
    // the boss leaves `cands` entirely — killed (mask bit set), skipped, or
    // turned to a corpse — all handled by the caller's candidate filtering, so
    // a gone boss simply isn't here to match.
    //
    // ADVANCE-FORWARD. When the commit releases (the boss we were heading to is
    // gone), we head to the next boss *after* it in encounter order, not to the
    // lowest-index survivor. Snapping to the lowest index is correct only when
    // the party clears strictly from boss #1; a party that started mid-list
    // (e.g. via a manual `dungeon clear boss #3` selection, or by skipping
    // early bosses) would otherwise walk back to boss #1 on every kill. We take
    // the candidates with encounter index strictly greater than the boss we
    // just left and pick the lowest-index one; only if nothing remains ahead do
    // we wrap to the full set to mop up any lower-index bosses left behind.
    //
    // STRICTLY ORDINAL — NO REACHABILITY RE-RANK. We pick by encounter index
    // alone and never skip a lower-index boss because a bounded path probe
    // reads it "unreachable." That probe tests the boss's STATIC spawn anchor,
    // which for pool-spawn bosses (the Wailing Caverns Disciples — Anacondra,
    // Cobrahn, Pythas, Serpentis) is an arbitrary pooled anchor that usually
    // isn't where the live boss stands, so it routinely probes as NOPATH and
    // the old "first reachable" pick skipped straight past the real next boss
    // to a far one with a good anchor (e.g. killing Anacondra jumped the target
    // to Verdan the Everliving, the last boss). Sequential in-order selection
    // is the contract: Advance walks toward the chosen boss using its LIVE
    // coords when found (falling back to the static anchor only to stream the
    // grid in), and a boss that genuinely can't be reached is handled by
    // Advance's stall -> manual `dc skip` path, not by silently re-targeting.
    std::optional<DungeonBossInfo> PickTarget(std::vector<DungeonBossInfo>& cands,
                                              uint32 stickyEntry,
                                              uint32 stickyEncounterIndex,
                                              bool haveStickyIndex)
    {
        if (cands.empty())
            return std::nullopt;

        // Hold the commit if the boss is still in the candidate list.
        if (stickyEntry)
            for (DungeonBossInfo const& info : cands)
                if (info.entry == stickyEntry)
                    return info;

        // `cands` arrives in encounter-index order, so the first match is
        // always the lowest-index one.

        if (haveStickyIndex)
        {
            // Finish any remaining anchors that SHARE the index we just left
            // before advancing past it. A single gate can be expressed as
            // several same-index objectives (Sunken Temple's six forcefield
            // defenders all sit at index 0, each on its own balcony that needs
            // its own boss-nav anchor); the old strict-greater advance picked
            // the next HIGHER index and stranded the siblings, dropping the gate
            // to a later wrap-around. `cands` is in clear order, so the first
            // equal-index match is the next sibling to clear. This also fixes
            // the boss-tied-to-objective case (e.g. Morphaz, a boss at index 5,
            // sharing its slot with the Weaver & Dreamscythe objective): the
            // boss is now picked right after its objective instead of last.
            for (DungeonBossInfo const& info : cands)
                if (BossOrderKey(info) == stickyEncounterIndex)
                    return info;

            // Commit released and the shared slot is done: head to the
            // lowest-index boss strictly after the one we just left.
            for (DungeonBossInfo const& info : cands)
                if (BossOrderKey(info) > stickyEncounterIndex)
                    return info;
        }

        // Fresh selection (no prior commit) or wrap-around (nothing ahead):
        // lowest-index survivor.
        return cands.front();
    }
}

std::optional<DungeonBossInfo> NextDungeonBossValue::Calculate()
{
    if (!bot || !bot->IsInWorld())
        return std::nullopt;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return std::nullopt;

    // New-instance reset. The run-scoped completion state read below — cleared
    // anchors (where a travel objective / conditional event latches its "done"),
    // user skips, the boss commit, and seen-boss bookkeeping — lives in the bot's
    // CONTEXT, not the instance, so it survives leaving the group and re-entering
    // a fresh instance. The completed-encounter MASK self-resets with the instance
    // (so real bosses come back), but these sets don't, and `dc on` is the only
    // other thing that clears them. Without this, re-running the same dungeon in a
    // new instance without toggling dc on inherits the prior run's latched
    // objectives — a completed ZulFarrak Temple event read "Done" again on
    // re-entry. Wipe them once when the run crosses into a different instance
    // (instanceId 0 = not in an instance / mid-load, never triggers it).
    uint32 const instanceId = bot->GetInstanceId();
    uint32 const lastRunInstance = AI_VALUE(uint32, "dungeon clear run instance");
    if (instanceId != 0 && lastRunInstance != 0 && lastRunInstance != instanceId)
    {
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear cleared anchors")->Get().clear();
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear skipped")->Get().clear();
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear seen bosses")->Get().clear();
        context->GetValue<uint32>("dungeon clear sticky boss")->Set(0u);
        context->GetValue<uint32>("dungeon clear selected boss")->Set(0u);
    }
    if (instanceId != 0 && lastRunInstance != instanceId)
        context->GetValue<uint32>("dungeon clear run instance")->Set(instanceId);

    std::vector<DungeonBossInfo> const& bosses =
        AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");

    std::unordered_set<uint32> const& skipped =
        AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped");

    // Travel objectives (BossRosterRegistry) have no kill-bit; once reached they
    // are latched here by DcObjectiveArriveAction so they drop out of the
    // candidate list exactly like a killed boss.
    std::unordered_set<uint32> const& cleared =
        AI_VALUE(std::unordered_set<uint32>&, "dungeon clear cleared anchors");

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
    InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
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
                if (cleared.count(info.entry))
                    invalid = true;
                else if (info.kind == DungeonAnchorKind::Boss &&
                         info.encounterIndex < 32 && (completedMask & (1u << info.encounterIndex)))
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

    // Build the candidate list in DBC encounter order (the order `bosses`
    // already arrives in). A boss is a candidate unless it's authoritatively
    // finished or deliberately passed over:
    //   - completion bit set — the group killed it,
    //   - user-skipped,
    //   - present but dead — a corpse whose completion bit hasn't flipped yet;
    //     transient, so we don't re-engage it.
    // A boss whose grid simply hasn't streamed in yet (not present at all) is
    // STILL a candidate, in its natural encounter-order slot.
    //
    // This list used to be split into alive-vs-not-spawned tiers, with the
    // not-spawned tier consulted only when nothing was spawned. That made the
    // bot skip past the next in-order boss (whose grid wasn't loaded) to a far
    // boss that merely happened to be spawned already — e.g. in Stratholme,
    // killing Forresten (idx 1) jumped straight to Barthilas (idx 10) because
    // the live-side bosses between them hadn't streamed in. Advance already
    // handles a not-yet-spawned target correctly: it walks toward the boss's
    // static spawn coords to stream the grid in, and only stalls (prompting a
    // manual `dc skip`) once close enough that the grid is certainly loaded and
    // the boss is genuinely absent (a true event-gate). So in-order selection
    // across spawned and not-yet-spawned bosses alike is safe, and keeps the
    // clear sequential. The empty-result == all-cleared contract that
    // AllClearedTrigger relies on still holds: the list empties only once every
    // boss is completed, skipped, or a fresh corpse.
    std::vector<DungeonBossInfo> cands;
    for (DungeonBossInfo const& info : bosses)
    {
        if (skipped.count(info.entry))
            continue;

        // Travel objective already reached this run (latched on arrival). Its
        // encounterIndex is just an ordering hint, not a real DBC bit, so it
        // must be filtered here rather than via the completion mask below.
        if (cleared.count(info.entry))
            continue;

        // Authoritative: this encounter is already complete in this instance.
        // Objectives never carry a real kill-bit, so the mask is consulted for
        // Boss anchors only (an objective's ordering index could otherwise
        // collide with an unrelated boss's set bit and vanish prematurely).
        if (info.kind == DungeonAnchorKind::Boss &&
            info.encounterIndex < 32 && (completedMask & (1u << info.encounterIndex)))
            continue;

        BossLiveState const state = LookupLive(liveness, info.entry);
        if (state.present && !state.alive)
            continue;  // corpse — transient, will flip to completed

        cands.push_back(info);
    }

    uint32 const stickyEntry = AI_VALUE(uint32, "dungeon clear sticky boss");

    // Resolve the committed boss's ORDER KEY from the full boss list (it stays
    // here even after the boss dies and drops out of the candidate list), so
    // PickTarget can advance to the next boss after it rather than snapping back
    // to the lowest survivor when the commit releases. Uses BossOrderKey (not the
    // raw encounterIndex) so a reordered boss advances along its path slot.
    uint32 stickyEncounterIndex = 0;
    bool haveStickyIndex = false;
    if (stickyEntry)
        for (DungeonBossInfo const& info : bosses)
            if (info.entry == stickyEntry)
            {
                stickyEncounterIndex = BossOrderKey(info);
                haveStickyIndex = true;
                break;
            }

    std::optional<DungeonBossInfo> pick =
        PickTarget(cands, stickyEntry, stickyEncounterIndex, haveStickyIndex);

    // Record the commit so the next computation holds it. Storing 0 on an empty
    // result releases the commit cleanly when the dungeon is cleared or every
    // boss is skipped.
    context->GetValue<uint32>("dungeon clear sticky boss")->Set(pick ? pick->entry : 0u);
    return pick;
}
