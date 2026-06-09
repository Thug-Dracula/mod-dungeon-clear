/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcPullPlanner.h"

#include "DungeonClearUtil.h"   // DC_PULL_* macros + DcTargeting::FindPullTarget (until DcTargeting moves)

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
    // True only when a COMPLETE navmesh route (PATHFIND_NORMAL) exists from the
    // bot to `p`. Mirrors ComputeCorridor's gate exactly. The camp helpers pick
    // points off the breadcrumb trail, but a trail can span a navmesh seam (a
    // drop-down, a ledge, a doorway) that is short in plan view yet not walkable
    // in a straight line. The move to such a point falls back to a straight
    // spline that clips terrain — the "tank runs under the map" symptom. Probing
    // the candidate with the same PathGenerator the move itself uses guarantees
    // every committed camp is reachable over a generated path. Bounded cost: one
    // Detour query per probe, and callers probe only points they would return.
    bool IsNavReachable(Player* bot, Position const& p)
    {
        if (!bot)
            return false;
        PathGenerator gen(bot);
        gen.CalculatePath(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ());
        return gen.GetPathType() == PATHFIND_NORMAL;
    }
}

Position DcPullPlanner::ComputeCampSlot(Player* bot, Position const& camp)
{
    if (!bot)
        return camp;

    // Deterministic per-bot offset so the slot is identical every tick: MoveTo
    // dedups an unchanging destination, so the follower glides to one spot and
    // parks there instead of re-pathing to a fresh random point each tick (which
    // would read as a nervous shuffle). Spread the party with the golden angle so
    // even a handful of bots fan out evenly around the anchor rather than
    // overlapping, and pick a 1-2yd radius for the requested gentle variance.
    uint32 const seed = static_cast<uint32>(bot->GetGUID().GetCounter());
    float const angle = static_cast<float>(seed) * 2.39996323f;  // golden angle (rad)
    float const radius = 1.0f + static_cast<float>(seed % 101) / 100.0f;  // [1.0, 2.0]

    float const fx = camp.GetPositionX() + radius * std::cos(angle);
    float const fy = camp.GetPositionY() + radius * std::sin(angle);
    float const fz = camp.GetPositionZ();

    // Route the fuzzed point through the navmesh. PathGenerator clamps an
    // off-mesh destination to the nearest walkable poly, so its actual endpoint
    // is guaranteed to be inside the zone geometry — the slot can never land in a
    // wall or off a ledge. Reject a probe that found no real ground path or had
    // to snap the endpoint far off the mesh, and reject a snap that dragged the
    // slot well past the intended 1-2yd (camp wedged against geometry); in those
    // cases fall back to the exact camp so we never displace a follower onto a
    // worse spot than the anchor itself.
    PathGenerator gen(bot);
    gen.CalculatePath(fx, fy, fz, /*forceDest*/ false);
    if (gen.GetPathType() & (PATHFIND_NOPATH | PATHFIND_FARFROMPOLY))
        return camp;

    G3D::Vector3 const end = gen.GetActualEndPosition();
    Position const slot(end.x, end.y, end.z, camp.GetOrientation());
    if (camp.GetExactDist(&slot) > 3.0f)
        return camp;
    return slot;
}
bool DcPullPlanner::ClassifyPullAdvanced(PlayerbotAI* botAI, Unit* target)
{
    if (!botAI || !target)
        return false;
    Player* bot = botAI->GetBot();
    if (!bot)
        return false;
    AiObjectContext* ctx = botAI->GetAiObjectContext();

    // Packmate radius — mobs this close to the target are its own pack and come
    // along regardless; also a search/broad-phase pad. (ComputeSafeCamp uses the
    // same value, so "pack" means the same thing across the feature.)
    constexpr float kPackRadius = 12.0f;

    // Dynamic verdict = estimate how many mobs aggro if we Leeroy on top of the
    // target, then compare to the party's Leeroy ceiling. The reach that decides
    // who aggros is each mob's OWN aggro radius (resolved per mob below), not a
    // hand-tuned distance, so the decision self-tunes per zone/level.
    uint32 const maxLeeroy = DcSettings::GetUInt(bot, "PullDynamicMaxLeeroyMobs");
    float const combatSpread = DcSettings::GetFloat(bot, "PullCombatSpread");
    // The assist-hop reach is the ENGINE's own value, not a DC knob: a creature's
    // CallAssistance() pulls same-faction help within CONFIG_CREATURE_FAMILY_-
    // ASSISTANCE_RADIUS. Reading it directly means the estimate tracks whatever the
    // realm actually uses for assist propagation, with nothing to retune per zone.
    float const assistRadius =
        sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);

    // Aggro reach is computed against the LOWEST-LEVEL living party member: when
    // the party piles onto the target in a Leeroy, that squishiest body attracts
    // proximity aggro from the farthest out, so it is the unit whose presence at
    // the camp decides who pulls. Fall back to the bot (solo, or no lower member).
    Player* lowMember = bot;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || !m->IsAlive() || m->GetMapId() != bot->GetMapId())
                continue;
            if (m->GetLevel() < lowMember->GetLevel())
                lowMember = m;
        }
    }

    // Candidate hostiles: gather them with a server-side grid search centred on the
    // PACK (target), NOT on the bot's own scan lists. The bot-cached "far targets"
    // value is range-limited to the bot, throttled (500ms), and was the root of the
    // late-ADVANCED flip: a neighbour pack near the target but on the far side of the
    // tank's approach only entered the bot's view as it walked up, so the verdict
    // resolved at point-blank instead of from first sight. A search around the target
    // sees the whole neighbourhood the instant the pack is picked, regardless of where
    // the tank stands or what it can see — the classification becomes a property of
    // the ROOM, stable across the entire approach. Radius must cover the farthest
    // mob the estimate could count: a proximity mob at the aggro-reach ceiling
    // (~45yd notice + reach) plus combat drift, and an assist buddy one ring
    // beyond that, so nothing the math could count is missed.
    constexpr float kMaxAggroReach = 50.0f;  // GetAttackDistance clamp (45) + reach
    float const searchRadius = kMaxAggroReach + combatSpread + assistRadius + kPackRadius;
    std::list<Creature*> nearby;
    Acore::AnyUnitInObjectRangeCheck check(target, searchRadius);
    Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(
        target, nearby, check);
    Cell::VisitObjects(target, searcher, searchRadius);

    std::vector<Unit*> hostiles;
    hostiles.reserve(nearby.size() + 1);
    for (Creature* c : nearby)
    {
        if (!c || !c->IsAlive() || !bot->IsHostileTo(c))
            continue;
        // Critters / non-combat pets / passive ambient never form a pull pack or a
        // chaining add — exclude them so they can't pad the pack-size count.
        if (c->IsCritter() || c->IsTotem())
            continue;
        hostiles.push_back(c);
    }
    // The target itself must be in the set so its pack is well-defined even if the
    // grid visitor somehow skipped it (despawn race / off-grid edge).
    if (std::find(hostiles.begin(), hostiles.end(), target) == hostiles.end())
        hostiles.push_back(target);

    // Whether a NEIGHBOUR mob counts toward the aggro estimate is gated three
    // ways, all measured PACK->neighbour (target as origin) so the verdict is a
    // stable property of the room, independent of where the tank is standing:
    //
    //   1. Line of sight (static VMAP). A Leeroy fight is STATIONARY at the camp,
    //      so a mob proximity-aggros only if it can actually SEE the party there —
    //      a pillar or wall between the camp and the mob means it never pulls.
    //      Scarlet Cathedral is the textbook case: its nave is full of pillars, so
    //      pairs 12-17yd apart (well inside aggro reach) do NOT chain because the
    //      pillars block sight; gating on LOS keeps those a 2-mob Leeroy instead of
    //      counting six through the stone. (This is the opposite choice from the old
    //      pack-adjacency model, which deliberately ignored LOS because it asked a
    //      different question — "will this pack drag in as the MOVING tank rounds the
    //      corner". The estimate asks "who aggros a fight that stays put", and for
    //      that, sight is exactly the right test.)
    //   2. No closed door between (VMAP LOS treats an open door's frame as clear and
    //      a shut door is a game object, not static geometry, so doors need their
    //      own test).
    //   3. Navmesh connectivity: a complete PATHFIND_NORMAL route within a
    //      corner-slack budget, so a mob that is visible across an unwalkable gap
    //      (a chasm, a ledge) and could never join the melee does not count.
    //
    // Formation/linked packmates bypass this gate entirely — they pull atomically
    // by packId (seed + closure), so a formation member behind a pillar still
    // counts. The gate only decides whether an UNRELATED neighbour aggros.
    //
    // Cost is bounded: every candidate was already gathered within searchRadius of
    // the target; the LOS ray and (only if it passes) the path query run for a
    // handful of mobs, often none.
    float const chainPathBudget = searchRadius * 1.5f;
    auto chainConnected = [&](Unit* u) -> bool
    {
        if (target->GetExactDist2d(u) > searchRadius)
            return false;
        // Static-geometry sight: pillars/walls block proximity aggro. VMAP-only
        // (LINEOFSIGHT_CHECK_VMAP) so an open door doesn't read as blocking and so
        // doors are left to the dedicated test below.
        if (!target->IsWithinLOSInMap(u, VMAP::ModelIgnoreFlags::Nothing,
                                      LINEOFSIGHT_CHECK_VMAP))
            return false;
        if (DcEngageGeometry::ClosedDoorBetween(target, u->GetPositionX(), u->GetPositionY(),
                              u->GetPositionZ()))
            return false;
        PathGenerator gen(target);
        gen.CalculatePath(u->GetPositionX(), u->GetPositionY(), u->GetPositionZ());
        if (gen.GetPathType() != PATHFIND_NORMAL)
            return false;
        Movement::PointsArray const& pts = gen.GetPath();
        float len = 0.0f;
        for (std::size_t k = 1; k < pts.size(); ++k)
            len += (pts[k] - pts[k - 1]).length();
        return len <= chainPathBudget;
    };

    // Engine-pack identity: tag each hostile with a small `packId` (0 = none)
    // derived from what the engine says actually pulls together, so the pure
    // classifier can cluster a strung-out formation as one pack and size it up
    // correctly instead of seeing several lone mobs. Pure geometry both over- and
    // under-clusters; the engine knows the truth.
    //   - Creature formations (GetFormation()->GetId()): mobs that move and aggro
    //     as a unit. The formation group id is already unique and stable, so
    //     formation-mates share it directly with no extra bookkeeping.
    //   - creature_linked_respawn links: a spawn/respawn dependency that in
    //     practice also marks "spawned and pulled together". Used only as a
    //     SECONDARY signal — it unions two ends ONLY when neither already belongs
    //     to a formation pack, so a respawn link can never merge or split a real
    //     formation. Most trash has neither and stays packId 0 (geometry-only).
    std::vector<uint32> packIds(hostiles.size(), 0u);
    std::unordered_map<uint32, uint32> formationToPack;  // formation group id -> packId
    uint32 nextPackId = 1;
    std::unordered_map<ObjectGuid, std::size_t> idxByGuid;
    for (std::size_t i = 0; i < hostiles.size(); ++i)
        idxByGuid[hostiles[i]->GetGUID()] = i;
    // Stage 1: formations (authoritative).
    for (std::size_t i = 0; i < hostiles.size(); ++i)
    {
        Creature* c = hostiles[i]->ToCreature();
        if (!c)
            continue;
        CreatureGroup const* grp = c->GetFormation();
        if (!grp)
            continue;
        uint32 const gid = grp->GetId();
        auto const it = formationToPack.find(gid);
        packIds[i] = (it != formationToPack.end()) ? it->second
                                                   : (formationToPack[gid] = nextPackId++);
    }
    // Stage 2: linked respawn (secondary). Only bridges two candidates when at
    // least one carries no formation pack yet, and never overwrites an existing
    // formation pack — formations stay authoritative.
    for (std::size_t i = 0; i < hostiles.size(); ++i)
    {
        Creature* c = hostiles[i]->ToCreature();
        if (!c)
            continue;
        ObjectGuid const linked = sObjectMgr->GetLinkedRespawnGuid(c->GetGUID());
        if (!linked)
            continue;
        auto const jt = idxByGuid.find(linked);
        if (jt == idxByGuid.end())
            continue;
        std::size_t const j = jt->second;
        if (packIds[i] && packIds[j])
            continue;  // both already (formation-)packed — leave them be
        uint32 const pid = packIds[i] ? packIds[i]
                         : packIds[j] ? packIds[j]
                                      : nextPackId++;
        packIds[i] = pid;
        packIds[j] = pid;
    }

    // Resolve game state (positions, per-mob aggro reach, the connectivity/door
    // gate, and the engine pack id) here, then hand the pure aggro-count estimate
    // to DungeonClearMath::EstimateAggroCount (unit-tested). The gate uses the
    // target itself as the pack's stand-in: packmates sit within a pack radius of
    // it, so connectivity to the target ~= connectivity to the pack, and it keeps
    // eligibility precomputable before the estimate.
    std::vector<DungeonClearMath::DynPullMob> mobs;
    mobs.reserve(hostiles.size());
    std::size_t targetIdx = 0;
    for (std::size_t i = 0; i < hostiles.size(); ++i)
    {
        Unit* u = hostiles[i];
        if (u == target)
            targetIdx = i;
        bool const chainEligible = u != target && chainConnected(u);
        // Per-mob aggro reach vs the squishiest body: the same core value the
        // commit-range / boss handoff use (GetAggroRange + reach, level-diff
        // scaled). Players/totems keep 0 — they never form a pull pack.
        float aggroReach = 0.0f;
        if (Creature* c = u->ToCreature())
            aggroReach = c->GetAggroRange(lowMember) + c->GetCombatReach();
        mobs.push_back({u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(),
                        chainEligible, packIds[i], aggroReach});
    }

    // DC_Z_LEVEL_TOLERANCE: a mob more than this far above/below is on another
    // floor (a ledge/ramp) — it must not seed the pack or count as a proximity /
    // assist aggro just because it is near in plan view. Same constant the
    // corridor/boss-arrival floor guards use elsewhere in this file.
    std::vector<std::size_t> counted;
    uint32 const count = DungeonClearMath::EstimateAggroCount(
        mobs, targetIdx, combatSpread, assistRadius, DC_Z_LEVEL_TOLERANCE, &counted);
    bool const advanced = count > maxLeeroy;
    DC_PULL_DEBUG("[DC:{}] dynamic: estimated {} aggro on target {} among {} hostiles "
                  "within {:.0f}yd (low-lvl {}, spread {:.0f}, assist {:.0f}, "
                  "ceiling {}) -> {}",
                  bot->GetName(), count, target->GetGUID().ToString(), mobs.size(),
                  searchRadius, uint32(lowMember->GetLevel()), combatSpread,
                  assistRadius, maxLeeroy, advanced ? "ADVANCED" : "LEEROY");
    // On the surprising verdict (Advanced), dump every hostile the estimate saw —
    // distance to the camp, its computed aggro reach, the eligibility gate, and
    // whether it was COUNTED — so a wrong count can be traced to the exact mobs and
    // the reason (proximity reach too far / assist hop / LOS gate / pack id).
    if (advanced)
    {
        std::unordered_set<std::size_t> countedSet(counted.begin(), counted.end());
        for (std::size_t i = 0; i < mobs.size(); ++i)
        {
            Unit* u = hostiles[i];
            Creature* cr = u->ToCreature();
            DC_PULL_DEBUG("[DC:{}]   mob[{}] entry {} lvl {} at {:.1f}yd "
                          "reach {:.1f}+{:.0f}={:.1f} elig {} pack {} -> {}",
                          bot->GetName(), i, u->GetEntry(),
                          cr ? uint32(cr->GetLevel()) : 0u, target->GetExactDist2d(u),
                          mobs[i].aggroReach, combatSpread,
                          mobs[i].aggroReach + combatSpread,
                          mobs[i].chainEligible ? "Y" : "N", mobs[i].packId,
                          countedSet.count(i) ? "COUNTED" : "-");
        }
    }
    return advanced;
}
void DcPullPlanner::UpdateDynamicPullMode(PlayerbotAI* botAI, AiObjectContext* context)
{
    if (!botAI || !context)
        return;
    Player* bot = botAI->GetBot();
    if (!bot)
        return;

    // Off / On are driven by DcPullAction; only Dynamic auto-decides per pack.
    if (context->GetValue<uint32>("dungeon clear pull setting")->Get() != 2u)
        return;

    DcPullContext& pull = context->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    bool const curBool = context->GetValue<bool>("dungeon clear pull mode")->Get();

    // Never flip the verdict mid-engagement: in combat or any non-Idle pull phase
    // the standing decision is latched until the fight resolves.
    if (bot->IsInCombat() || pull.phase != DcPullPhase::Idle)
        return;

    auto apply = [&](bool want, uint32 decision)
    {
        pull.decision = decision;
        if (want == curBool)
            return;
        context->GetValue<bool>("dungeon clear pull mode")->Set(want);
        DcLeaderSignal::SetLeaderDazeImmunity(bot, want);
        // Switching to Advanced: seed a camp at the tank so followers have an
        // immediate hold point (mirrors DcPullAction's On activation); the pull
        // pipeline overwrites it with the real safe camp on commit.
        if (want)
            pull.camp =
                Position(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    };

    std::optional<DungeonBossInfo> next =
        context->GetValue<std::optional<DungeonBossInfo>>("next dungeon boss")->Get();
    Unit* target = next.has_value() ? DcTargeting::FindPullTarget(botAI, *next) : nullptr;
    if (!target)
    {
        // Nothing to size up: fall back to the Leeroy ladder and drop the latch.
        pull.decisionTarget = ObjectGuid::Empty;
        apply(false, 0u);
        return;
    }

    // Per-pack latch, UPGRADE-ONLY while approaching the SAME pack.
    //
    // ClassifyPullAdvanced now sizes up the pack with a search centred on the PACK
    // itself (positions, navmesh paths and door tests all measured from the target,
    // never from the tank), so the verdict is a stable property of the room: it reads
    // the SAME from 35yd out as from point-blank. The first-sight decision is therefore
    // already correct and no longer needs to be chased down as the tank closes — the
    // old "neighbour only resolves at the last second" blind spot is gone. We still
    // re-check on a throttle and allow a Leeroy verdict to UPGRADE to Advanced (never
    // downgrade), but only to catch genuine CHANGES in the room — a patrol wandering
    // into chain range, a pack pulled by another fight — not to compensate for our own
    // movement. Once Advanced it is locked for the rest of the approach. One stable,
    // non-flapping decision per pack; the party is never churned Leeroy<->Advanced.
    constexpr uint32 kRecheckMs = 400;
    bool const sameTarget = (pull.decisionTarget == target->GetGUID());

    if (sameTarget)
    {
        // Already Advanced for this pack: locked, never reconsidered.
        if (curBool)
            return;
        // Standing Leeroy: re-check on a throttle, upgrade-only.
        uint32 const now = getMSTime();
        if (pull.decisionSince != 0 && (now - pull.decisionSince) < kRecheckMs)
            return;
        pull.decisionSince = now;
        if (ClassifyPullAdvanced(botAI, target))
        {
            apply(true, 2u);
            DC_PULL_INFO("[DC:{}] dynamic verdict UPGRADED for pack {}: LEEROY -> "
                         "ADVANCED (room changed — patrol/neighbour entered chain range)",
                         bot->GetName(), target->GetGUID().ToString());
        }
        return;
    }

    // New pack: size it up fresh and stamp the latch + re-check clock.
    bool const advanced = ClassifyPullAdvanced(botAI, target);
    pull.decisionTarget = target->GetGUID();
    pull.decisionSince = getMSTime();
    apply(advanced, advanced ? 2u : 1u);
    DC_PULL_INFO("[DC:{}] dynamic verdict for pack {}: {}", bot->GetName(),
                 target->GetGUID().ToString(), advanced ? "ADVANCED" : "LEEROY");
}
std::optional<Position> DcPullPlanner::ComputeSafeCamp(PlayerbotAI* botAI, Unit* target,
                                                          float setback, float safeRadius,
                                                          float maxDrag,
                                                          float& clearanceOut, float& dragOut)
{
    clearanceOut = std::numeric_limits<float>::max();
    dragOut = 0.0f;
    if (!botAI || !target)
        return std::nullopt;
    Player* bot = botAI->GetBot();
    if (!bot)
        return std::nullopt;
    AiObjectContext* ctx = botAI->GetAiObjectContext();

    // Grouping radius: hostiles this close to the pulled target are its own
    // packmates — they come along to camp anyway, so they don't count against
    // camp clearance. Everything farther is an "other" pack we must stay clear of.
    constexpr float kPackRadius = 12.0f;
    // How far apart to sample candidate camp points walking back along the route.
    constexpr float kStep = 4.0f;
    if (maxDrag < setback)
        maxDrag = setback;

    // Ranged LOS-break: if the pulled target fights at range (caster / archer /
    // wand), the camp must additionally break line of sight to it so the rangers
    // are forced to close to melee at camp instead of plinking the party across the
    // room. We keep walking the camp back along the cleared route — usually to the
    // doorway/corner the tank entered through — until a point with no LOS to the
    // pack is found, allowing a larger drag (PullRangedMaxDrag) to reach it. Only
    // the CURRENT target's LOS is tested; its packmates cluster around it. When no
    // out-of-sight point is reachable the normal farthest-back fallback still wins.
    bool const losBreak = DcSettings::GetBool(bot, "PullRangedLosBreak") &&
                          DcEngageGeometry::IsRangedAttacker(bot, target);
    if (losBreak)
    {
        maxDrag = std::max(maxDrag, DcSettings::GetFloat(bot, "PullRangedMaxDrag"));
        DC_PULL_DEBUG("[DC:{}] safe-camp: target {} is a ranged attacker -> requiring "
                      "LOS break, maxDrag extended to {:.0f}yd",
                      bot->GetName(), target->GetGUID().ToString(), maxDrag);
    }
    // VMAP-static LOS from the pack to a candidate camp ground point: a wall or
    // pillar between them means a ranger there can't hit the camp and must move in.
    // VMAP-only (no dynamic objects) so an open door's frame doesn't read as cover —
    // matches the chain-aggro gate in ClassifyPullAdvanced.
    auto breaksLos = [&](Position const& p) -> bool
    {
        return losBreak &&
               !target->IsWithinLOS(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ(),
                                    VMAP::ModelIgnoreFlags::Nothing, LINEOFSIGHT_CHECK_VMAP);
    };

    // Resolve the other-pack hostiles once (alive, hostile, not the target, not a
    // packmate). Same candidate set the pull / trash scans use.
    GuidVector const& farTargets =
        ctx->GetValue<GuidVector>("dungeon clear far targets")->Get();
    GuidVector const& possibleTargets =
        ctx->GetValue<GuidVector>("possible targets")->Get();
    GuidVector const& candidates = farTargets.empty() ? possibleTargets : farTargets;

    std::vector<Unit*> others;
    others.reserve(candidates.size());
    for (ObjectGuid guid : candidates)
    {
        Unit* u = ObjectAccessor::GetUnit(*bot, guid);
        if (!u || !u->IsAlive() || u == target)
            continue;
        if (!bot->IsHostileTo(u))
            continue;
        // On another floor (a pack on a ledge/ramp directly above or below): it
        // can't aggro the camp fight, so it must not push the camp farther back.
        // Same height gate the Dynamic decision and the corridor scans use, so
        // placement and classification agree about what "near" means in 3D.
        if (std::fabs(u->GetPositionZ() - target->GetPositionZ()) > DC_Z_LEVEL_TOLERANCE)
            continue;
        if (u->GetExactDist2d(target) <= kPackRadius)
            continue;  // packmate — fought regardless of where camp sits
        others.push_back(u);
    }

    auto clearanceAt = [&others](Position const& p) -> float
    {
        float nearest = std::numeric_limits<float>::max();
        for (Unit* u : others)
            nearest = std::min(nearest, p.GetExactDist2d(u));
        return nearest;
    };

    std::vector<Position> const& crumbs =
        ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get().breadcrumbs;

    Position const tankPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    // --- Preferred: walk BACK along the breadcrumb trail ------------------
    // The trail is the ground the tank actually walked (oldest -> newest), so it
    // stays valid even though the pull drag-back resets the long-path cursor (the
    // bug that made a cursor-based "point behind" find only a few yards after the
    // first pull). Accumulate corridor distance from the tank backward through it.
    // Dungeon mobs have no leash, so take the first point at least `setback` back
    // that also clears safeRadius, walking further (up to maxDrag) only if a
    // neighbour is still within safeRadius. A gap bigger than kJumpGuard means the
    // trail isn't contiguous there (a drag/teleport boundary) — stop, nothing
    // beyond it is really "behind us". Track the farthest point as the fallback.
    constexpr float kJumpGuard = 12.0f;
    Position best = tankPos;
    float bestClear = clearanceAt(tankPos);
    float bestDrag = 0.0f;
    float bestAlong = 0.0f;
    // Best reachable LOS-breaking point (ranged pulls only): tracked separately so
    // that if no point satisfies BOTH clearance and LOS-break at the full setback,
    // we still prefer the farthest out-of-sight point over a closer in-sight one —
    // breaking the rangers' line is the whole goal even at imperfect clearance.
    Position bestLos = tankPos;
    float bestLosClear = std::numeric_limits<float>::max();
    float bestLosDrag = 0.0f;
    float bestLosAlong = -1.0f;  // <0 => none found
    Position prev = tankPos;
    float along = 0.0f;
    for (std::size_t i = crumbs.size(); i-- > 0; )
    {
        Position const& c = crumbs[i];
        // 3D segment length: this is the real walked distance, and it makes the
        // discontinuity guard catch a vertical jump (a drop-down / ledge) that a
        // 2D measure would miss — the trail must stay contiguous in space, not
        // just in plan view, or a later camp pick lands on the wrong floor.
        float const seg = prev.GetExactDist(&c);
        prev = c;
        if (seg > kJumpGuard)
            break;  // discontinuity behind us — stop here
        along += seg;
        // Only ever return / fall back to a crumb the move can reach over a
        // complete generated path. A crumb within kJumpGuard but across a
        // navmesh seam would otherwise be committed and the move to it would
        // straight-line under the map.
        if (!IsNavReachable(bot, c))
            continue;
        float const clear = clearanceAt(c);
        float const drag = tankPos.GetExactDist(&c);
        bool const breaks = breaksLos(c);
        if (along > bestAlong)  // farthest reachable back so far (fallback)
        {
            best = c;
            bestClear = clear;
            bestDrag = drag;
            bestAlong = along;
        }
        if (breaks && along > bestLosAlong)  // farthest reachable out-of-sight point
        {
            bestLos = c;
            bestLosClear = clear;
            bestLosDrag = drag;
            bestLosAlong = along;
        }
        // Accept the first point that is far enough back, clears other packs, AND
        // (for a ranged pull) breaks LOS to the pack.
        if (along >= setback && clear >= safeRadius && (!losBreak || breaks))
        {
            clearanceOut = clear;
            dragOut = drag;
            return c;
        }
        if (along >= maxDrag)
            break;
    }

    // Ranged pull, no point cleared every gate: take the farthest out-of-sight
    // point we did find (at least half the setback back), since forcing the rangers
    // to close matters more than perfect pack clearance.
    if (losBreak && bestLosAlong >= setback * 0.5f)
    {
        clearanceOut = bestLosClear;
        dragOut = bestLosDrag;
        return bestLos;
    }

    // Got a meaningful distance back along the trail (at least half the setback):
    // use the farthest point even if a neighbour is within safeRadius — no leash,
    // so far-and-imperfect beats close. (For a ranged pull with no reachable
    // out-of-sight point, this is the graceful "room has no cover" fallback.)
    if (bestAlong >= setback * 0.5f)
    {
        clearanceOut = bestClear;
        dragOut = bestDrag;
        return best;
    }

    // --- Fallback: cleared route behind is too short (e.g. first pull) ------
    // Pull straight back, directly away from the target, snapped to the navmesh.
    // Try the full setback first, shrinking until a walkable point is found.
    float dx = tankPos.GetPositionX() - target->GetPositionX();
    float dy = tankPos.GetPositionY() - target->GetPositionY();
    float const len = std::sqrt(dx * dx + dy * dy);
    if (len > 0.1f)
    {
        dx /= len;
        dy /= len;
        for (float d = setback; d >= kStep; d -= kStep)
        {
            NavmeshSnap::Result snap = NavmeshSnap::Snap(
                bot, tankPos.GetPositionX() + dx * d,
                tankPos.GetPositionY() + dy * d, tankPos.GetPositionZ(), 10.0f);
            if (!snap.ok)
                continue;
            Position cand(snap.x, snap.y, snap.z);
            // The straight-back projection ignores walls: the snapped point can
            // land on-mesh but on the far side of a wall / on another level. Only
            // keep it if a complete generated path reaches it, so the move never
            // straight-lines through the geometry in between.
            if (!IsNavReachable(bot, cand))
                continue;
            float const c = clearanceAt(cand);
            float const drag = tankPos.GetExactDist(&cand);
            if (drag > bestDrag)
            {
                best = cand;
                bestClear = c;
                bestDrag = drag;
            }
            if (c >= safeRadius)
            {
                clearanceOut = c;
                dragOut = drag;
                return cand;
            }
        }
    }

    clearanceOut = bestClear;
    dragOut = bestDrag;
    return best;
}
std::optional<Position> DcPullPlanner::ComputeTrailCamp(PlayerbotAI* botAI,
                                                           float setback, float maxDrag)
{
    if (!botAI)
        return std::nullopt;
    Player* bot = botAI->GetBot();
    if (!bot)
        return std::nullopt;
    AiObjectContext* ctx = botAI->GetAiObjectContext();

    if (maxDrag < setback)
        maxDrag = setback;

    Position const tankPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    std::vector<Position> const& crumbs =
        ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get().breadcrumbs;

    // Walk BACK along the trail (newest -> oldest) accumulating corridor distance,
    // exactly like ComputeSafeCamp's preferred branch but without the clearance
    // gate: take the first point at least `setback` behind us, stopping at maxDrag
    // or a trail discontinuity (kJumpGuard). Track the farthest contiguous point
    // as the fallback when the trail is shorter than the full setback (e.g. right
    // after a fight, before the tank has laid much new trail).
    constexpr float kJumpGuard = 12.0f;
    Position best = tankPos;
    float bestAlong = 0.0f;
    Position prev = tankPos;
    float along = 0.0f;
    for (std::size_t i = crumbs.size(); i-- > 0; )
    {
        Position const& c = crumbs[i];
        // 3D segment length (see ComputeSafeCamp): true walked distance, and the
        // guard catches a vertical drop a 2D measure would treat as contiguous.
        float const seg = prev.GetExactDist(&c);
        prev = c;
        if (seg > kJumpGuard)
            break;  // discontinuity behind us — stop here
        along += seg;
        // Only trail to a crumb the party can reach over a complete generated
        // path — a seam crumb would make the follower move straight-line under
        // the map.
        if (!IsNavReachable(bot, c))
            continue;
        if (along > bestAlong)
        {
            best = c;
            bestAlong = along;
        }
        if (along >= setback)
            return c;
        if (along >= maxDrag)
            break;
    }

    // Trail too short to reach the full setback: trail the farthest point we have
    // (the party simply stacks closer behind the tank until more trail accrues).
    return best;
}
bool DcPullPlanner::IsPartySetAtCamp(Player* leader, Position const& camp, float setRadius)
{
    if (!leader)
        return false;
    Group* group = leader->GetGroup();
    if (!group)
        return true;  // solo tank — nobody to wait on

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == leader)
            continue;
        if (member->GetMapId() != leader->GetMapId())
            continue;
        if (member->isDead())
            continue;  // dead members handled by the party-died trigger
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;  // real player — never gate the pull on them
        if (member->GetExactDist2d(&camp) > setRadius)
            return false;
        // Healers are deliberately never held passive (ApplyFollowerPassive
        // exempts them so they can heal the tank through the drag-back), so
        // requiring the passive strategy from them here would wait out the full
        // Forming timeout on EVERY pull with a healer in the group — the gate
        // would block on a flag the camp-hold code is designed never to set.
        // Mirror that exemption: a healer counts as "set" on position alone;
        // every other member must also carry the passive strategy (parked AND
        // holding fire) before the party is ready to tag.
        if (!PlayerbotAI::IsHeal(member) &&
            !memberAI->HasStrategy("passive", BOT_STATE_COMBAT))
            return false;
    }
    return true;
}
