/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcEngageGeometry.h"

#include "DcDoorIndex.h"
#include "DungeonClearUtil.h"   // DcTargeting::GetLiveBoss (until DcTargeting moves)
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
#include "DBCStores.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
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

float DcEngageGeometry::AggroRangeOf(Player* bot, Unit* u, float fallback,
                                    float floorYd, float capYd)
{
    if (!bot || !u)
        return fallback;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return fallback;

    Creature* c = u->ToCreature();
    if (!c)
        return fallback;            // players/totems: keep the caller's band

    // Core formula: detection range adjusted by level diff, already clamped
    // 5-45yd. Add the creature's reach so the band measures "how close the
    // route must pass for it to pull", not just its notice distance.
    float range = c->GetAggroRange(bot) + c->GetCombatReach();
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}
float DcEngageGeometry::BossEngageRange(Player* bot, AiObjectContext* ctx,
                                        DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return staticRange;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return staticRange;

    Creature* live = DcTargeting::GetLiveBoss(bot, ctx, boss.entry);
    if (!live)
        return staticRange;         // not loaded yet — use the static fallback

    float const margin =
        DcSettings::GetFloat(bot, "AggroRangeMargin");
    float const floorYd =
        DcSettings::GetFloat(bot, "BossEngageRangeFloor");
    float const capYd =
        DcSettings::GetFloat(bot, "BossEngageRangeCap");

    // Hand off as the tank enters the boss's real aggro bubble: its notice
    // distance + both reaches + a small margin so the engage trigger fires
    // just before the boss would pull on its own.
    float range = live->GetAggroRange(bot) + live->GetCombatReach()
                + bot->GetCombatReach() + margin;
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}
float DcEngageGeometry::PullCommitRange(Player* bot, Unit* target, float staticRange)
{
    if (!bot || !target)
        return staticRange;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return staticRange;

    Creature* c = target->ToCreature();
    if (!c)
        return staticRange;             // players/totems: keep the fixed fallback

    float const margin = DcSettings::GetFloat(bot, "AggroRangeMargin");
    float const floorYd = DcSettings::GetFloat(bot, "PullCommitRangeFloor");
    float const capYd = DcSettings::GetFloat(bot, "PullCommitRangeCap");

    // Stop just as the tank would enter the pack's real aggro bubble: the core's own
    // notice distance (Creature::GetAggroRange, already level/config-scaled and
    // clamped 5-45yd) + both reaches + a small margin, so the commit fires a hair
    // BEFORE the pack would pull on its own. Identical formula to BossEngageRange.
    float range = c->GetAggroRange(bot) + c->GetCombatReach()
                + bot->GetCombatReach() + margin;
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}

std::optional<Position> DcEngageGeometry::AggroSafeApproachPoint(
    Player* bot, float bx, float by, float bz, float safeRadius, Unit* target)
{
    if (!bot || !target || safeRadius <= 0.0f)
        return std::nullopt;

    float const tx = bot->GetPositionX();
    float const ty = bot->GetPositionY();
    float const gx = target->GetPositionX();
    float const gy = target->GetPositionY();

    // Does the straight 2D approach bot->target already stay outside the boss's
    // aggro sphere? Then no detour — engage directly.
    if (!NeedsRoomAggroSkirt(tx, ty, gx, gy, bx, by, safeRadius))
        return std::nullopt;

    // Orbit the sphere: step the bot's current bearing (measured from the boss)
    // toward the target's bearing by a capped angular increment and place the
    // waypoint on the safe ring there. Called each tick this walks the tank around
    // the short arc; the early-out above ends the orbit the moment a straight shot
    // at the target clears the sphere.
    float const phi = std::atan2(ty - by, tx - bx);   // bot bearing from boss
    float const angG = std::atan2(gy - by, gx - bx);  // target bearing from boss
    // Shortest signed turn phi->angG, in (-pi, pi].
    float const delta = std::atan2(std::sin(angG - phi), std::cos(angG - phi));
    // ~34 degrees/tick keeps each leg short enough that it hugs the ring exterior.
    constexpr float kOrbitStep = 0.6f;
    float const step = std::clamp(delta, -kOrbitStep, kOrbitStep);
    float const wpBearing = phi + step;

    // A touch beyond the ring so the tank skirts the OUTSIDE of the aggro sphere.
    float const wpR = safeRadius * 1.15f + 1.0f;
    float const wx = bx + std::cos(wpBearing) * wpR;
    float const wy = by + std::sin(wpBearing) * wpR;

    NavmeshSnap::Result const snap = NavmeshSnap::Snap(bot, wx, wy, bz, 25.0f);
    if (!snap.ok)
        return std::nullopt;  // no walkable detour — fall back to a direct approach

    return Position(snap.x, snap.y, snap.z, 0.0f);
}

bool DcEngageGeometry::NeedsRoomAggroSkirt(float botX, float botY,
                                          float targetX, float targetY,
                                          float bossX, float bossY,
                                          float avoidRadius)
{
    if (avoidRadius <= 0.0f)
        return false;

    // Closest approach of the bot->target chord to the boss centre, squared
    // (no sqrt). Inside the padded radius -> the line clips the sphere -> skirt.
    float const clipSq = DungeonClearMath::DistSqToSegment2D(
        bossX, bossY, botX, botY, targetX, targetY);
    return clipSq < avoidRadius * avoidRadius;
}
bool DcEngageGeometry::IsAtBossEngage(Player* bot, AiObjectContext* ctx,
                                      DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return false;

    Creature* live = DcTargeting::GetLiveBoss(bot, ctx, boss.entry);
    float const bx = live ? live->GetPositionX() : boss.x;
    float const by = live ? live->GetPositionY() : boss.y;
    float const bz = live ? live->GetPositionZ() : boss.z;

    float const engageRange = BossEngageRange(bot, ctx, boss, staticRange);

    // Straight-line aggro-bubble gate (same value the trigger ladder reads).
    if (bot->GetDistance(bx, by, bz) >= engageRange)
        return false;

    // Same-floor fast path: within tolerance the straight-line distance is the
    // real approach distance (slopes/ramps stay under it), so trust the gate
    // above. Skips the path probe in the overwhelmingly common case.
    float const dz = std::fabs(bz - bot->GetPositionZ());
    if (dz <= DC_Z_LEVEL_TOLERANCE)
        return true;

    // Static coords (boss not loaded) are necessarily far and only reached on
    // the tank's own floor; no under/over-the-ledge case to guard against.
    if (!live)
        return true;

    // Vertically separated: a 3D-close boss may be a whole floor below or above
    // — the tank on a balcony/walkway directly over Rethilgore, or parked under
    // an upper-floor boss like Fenrus. Straight-line proximity is then a lie:
    // the real approach winds down stairs/ramps and the boss can neither be
    // pulled (no LOS / out of its own aggro band) nor does the tank ever reach
    // it, so the at-boss handoff fires and the run dead-stops in "Holding near".
    //
    // The old guard (IsLevelReachable) only asked whether SOME ground path to
    // the boss's level exists — which, in a connected dungeon, it almost always
    // does — so it failed to catch this. Require instead that the NAVIGATIONAL
    // distance to the boss is itself within engage range: a real arrival, not a
    // long detour down. A boss directly overhead clamps off-mesh (PathGenerator
    // snaps the destination back to the tank's own floor) and fails the
    // end-on-boss-level check; a balcony boss produces a long stairs path and
    // fails the length check. Either way the handoff defers and Advance keeps
    // walking the route to the boss's floor, where dz collapses and the pull
    // fires for real.
    PathGenerator gen(bot);
    gen.CalculatePath(bx, by, bz, /*forceDest*/ false);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;

    Movement::PointsArray const& path = gen.GetPath();
    if (path.empty())
        return false;
    if (std::fabs(path.back().z - bz) > DC_Z_LEVEL_TOLERANCE)
        return false;

    return gen.getPathLength() <= engageRange;
}
bool DcEngageGeometry::IsRangedAttacker(Player* bot, Unit* u)
{
    Creature const* c = u ? u->ToCreature() : nullptr;
    if (!c)
        return false;
    CreatureTemplate const* t = c->GetCreatureTemplate();
    if (!t)
        return false;

    // 1. Caster class. Creatures collapse all casters onto a small set of
    // classes (MAGE in practice); accept the others defensively in case a realm's
    // creature data tags a shadow/elemental caster as PRIEST/SHAMAN/WARLOCK.
    switch (t->unit_class)
    {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_WARLOCK:
            return true;
        default:
            break;
    }

    // 2. Ranged weapon in the virtual ranged slot. Bow / gun / crossbow / wand
    // mean the mob attacks from afar; thrown is excluded (short range — the mob
    // closes to throwing distance, which is effectively melee for our purposes).
    uint32 const rangedItemId =
        c->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + uint32(RANGED_ATTACK));
    if (ItemEntry const* ie = sItemStore.LookupEntry(rangedItemId))
    {
        if (ie->ClassID == ITEM_CLASS_WEAPON &&
            (ie->SubclassID == ITEM_SUBCLASS_WEAPON_BOW ||
             ie->SubclassID == ITEM_SUBCLASS_WEAPON_GUN ||
             ie->SubclassID == ITEM_SUBCLASS_WEAPON_CROSSBOW ||
             ie->SubclassID == ITEM_SUBCLASS_WEAPON_WAND))
            return true;
    }

    // 3. A damaging spell with real reach. Mirrors Creature::reachWithSpellAttack's
    // effect test (school damage / leech / instakill) but without the live mana and
    // distance gating — we want the mob's CAPABILITY, not whether it can cast right
    // now. A max range above the floor means it will plant and cast rather than run
    // in, which is the whole reason to break LOS. Catches a caster the engine tagged
    // with a melee unit_class.
    float const rangeFloor = DcSettings::GetFloat(bot, "PullRangedSpellRangeFloor");
    for (uint32 i = 0; i < MAX_CREATURE_SPELLS; ++i)
    {
        uint32 const spellId = t->spells[i];
        if (!spellId)
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;
        bool damaging = false;
        for (uint32 j = 0; j < MAX_SPELL_EFFECTS; ++j)
        {
            uint32 const eff = spellInfo->Effects[j].Effect;
            if (eff == SPELL_EFFECT_SCHOOL_DAMAGE || eff == SPELL_EFFECT_HEALTH_LEECH ||
                eff == SPELL_EFFECT_INSTAKILL || eff == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE)
            {
                damaging = true;
                break;
            }
        }
        if (!damaging)
            continue;
        if (spellInfo->GetMaxRange(false) > rangeFloor)
            return true;
    }

    return false;
}
bool DcEngageGeometry::IsDoorClosed(GameObject const* go)
{
    if (!go || !go->IsInWorld())
        return false;
    GameObjectTemplate const* info = go->GetGOInfo();
    if (!info || info->type != GAMEOBJECT_TYPE_DOOR)
        return false;
    // Authored non-blocking (decorative / always-passable) doors never count.
    if (info->door.ignoredByPathing)
        return false;
    // Physical truth on this core: a door's collision follows GOState ALONE —
    // GO_STATE_READY has collision ON (blocking), both ACTIVE states have it
    // OFF (passable). The template's startOpen flag plays no runtime role:
    // GameObject::AddToWorld and SetGoState both do
    // EnableCollision(state == GO_STATE_READY). The previous XOR against
    // startOpen misread every startOpen=1 gate spawned ACTIVE (the Stratholme
    // portcullises) as closed, parking the tank in front of an open gate.
    return go->GetGoState() == GO_STATE_READY;
}
bool DcEngageGeometry::ClosedDoorBetween(WorldObject* from, float tx, float ty,
                                         float tz, float /*corridorWidth*/)
{
    if (!from)
        return false;
    Map* map = from->GetMap();
    if (!map)
        return false;

    // GameObject-only line of sight. A door / gate is an M2 (or WMO) GameObject
    // whose collision is inserted into the map's DYNAMIC collision tree ONLY
    // while it is shut — GameObject toggles EnableCollision(state ==
    // GO_STATE_READY), so the instant a door opens its model leaves the tree.
    // Therefore a BLOCKED GameObject-LOS ray means a CLOSED door sits squarely
    // on the line between the two points, judged against the door's real
    // oriented collision mesh.
    //
    // This replaces the old GeoBox-AABB-band test, which was the root of the
    // door whack-a-mole: the AABB can sit several yards off where the navmesh
    // actually threads a doorway (SM Cathedral's Chapel Door) and can't tell a
    // route THROUGH a wide doorway from one running PAST it (SFK's courtyard
    // gate). A real LOS ray has neither failure mode and needs no per-door
    // tuning.
    //
    // VMAP (static walls/floor) is deliberately EXCLUDED: we only want "a closed
    // door is in the way", never "the target is around a static corner" — the
    // navmesh already routes around static geometry, so a static block is never
    // the dungeon-clear blocker, and excluding it stops a target behind a wall
    // beside an OPEN passage from being mistaken for door-blocked.
    //
    // Requires the realm's CheckGameObjectLoS=1 (set here); with it off the call
    // returns clear and the bot falls back to the navmesh's door-blind default.
    // Rays run ~2yd above each endpoint so they cross the door PANEL instead of
    // grazing the floor seam. corridorWidth is kept for ABI but unused — an LOS
    // ray needs no width band.
    return !map->isInLineOfSight(
        from->GetPositionX(), from->GetPositionY(), from->GetPositionZ() + 2.0f,
        tx, ty, tz + 2.0f, from->GetPhaseMask(),
        LINEOFSIGHT_CHECK_GOBJECT_ALL, VMAP::ModelIgnoreFlags::Nothing);
}
bool DcEngageGeometry::ClosedDoorNear(WorldObject* ref, float x, float y, float z,
                                      float radius)
{
    if (!ref)
        return false;
    Map* map = ref->GetMap();
    if (!map)
        return false;

    float const radiusSq = radius * radius;
    float const loZ = z - DC_DOOR_Z_BAND;
    float const hiZ = z + DC_DOOR_Z_BAND;

    // Cached door list; shut-state read fresh per door (see DcDoorIndex).
    for (ObjectGuid const guid : DcDoorIndex::Get(map))
    {
        GameObject* go = map->GetGameObject(guid);
        if (!IsDoorClosed(go))
            continue;

        float const gz = go->GetPositionZ();
        if (gz < loZ || gz > hiZ)
            continue;  // door on another floor — near in plan-view only

        float const dx = go->GetPositionX() - x;
        float const dy = go->GetPositionY() - y;
        if (dx * dx + dy * dy <= radiusSq)
            return true;
    }
    return false;
}
float DcEngageGeometry::DistAlongPathToClosedDoor(
    Player* bot, ChunkedPathfinder::Result const& path,
    float doorX, float doorY, float doorZ, float maxLookAhead)
{
    if (!bot || !path.reachable || path.segments.empty())
        return std::numeric_limits<float>::max();

    // Walk the smoothed polyline accumulating distance FROM THE PATH START, and
    // track two cursors: where the path is closest to the bot (the tank's current
    // progress along it) and where it first enters THIS door's band. Return the
    // forward gap between them — the travel still REMAINING to the doorway.
    //
    // The accumulate-from-the-bot version was wrong: the long-path is anchored
    // where it was built, which falls behind as the tank advances, so walking
    // from the bot counted the already-walked prefix and the returned distance
    // GREW as the tank closed in (it never dropped to the stand-off until the bot
    // physically entered the band, parking it on the door). Subtracting the bot's
    // progress cursor fixes that. Only this one door is tested: scanning every
    // nearby door returned whichever the winding route grazed first (an off-route
    // side door), masking the real blocker.
    float const bandSq = DC_DOOR_BAND * DC_DOOR_BAND;
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    float const zBand = DC_DOOR_Z_BAND;
    float prevX = 0.0f, prevY = 0.0f, prevZ = 0.0f;
    bool havePrev = false;
    float accumulated = 0.0f;     // distance from path start to current point
    float cursorAccum = 0.0f;     // accumulated at the point closest to the bot
    float bestBotDistSq = std::numeric_limits<float>::max();
    float doorAccum = -1.0f;      // accumulated at the band entry

    auto visit = [&](float px, float py, float pz) -> bool
    {
        if (havePrev)
        {
            // Only treat the door as on THIS leg if the leg shares its floor —
            // otherwise the route passing over/under the door (a stacked deck or
            // a ramp) registers a band entry far before the real doorway and
            // parks the tank short.
            bool const onFloor =
                doorZ >= std::min(prevZ, pz) - zBand &&
                doorZ <= std::max(prevZ, pz) + zBand;
            if (onFloor &&
                DungeonClearMath::DistSqToSegment2D(doorX, doorY, prevX, prevY, px, py) <= bandSq)
            {
                doorAccum = accumulated;   // at the START (prev) of the hitting segment
                return true;
            }
            float const dx = px - prevX;
            float const dy = py - prevY;
            accumulated += std::sqrt(dx * dx + dy * dy);
        }
        float const bdx = px - botX;
        float const bdy = py - botY;
        float const bd2 = bdx * bdx + bdy * bdy;
        if (bd2 < bestBotDistSq)
        {
            bestBotDistSq = bd2;
            cursorAccum = accumulated;
        }
        prevX = px;
        prevY = py;
        prevZ = pz;
        havePrev = true;
        return false;
    };

    bool hit = false;
    for (PathSegment const& seg : path.segments)
    {
        if (seg.polyline.empty())
            hit = visit(seg.ex, seg.ey, seg.ez);
        else
            for (G3D::Vector3 const& pt : seg.polyline)
                if ((hit = visit(pt.x, pt.y, pt.z)))
                    break;
        if (hit || accumulated >= maxLookAhead)
            break;
    }

    if (!hit)
        return std::numeric_limits<float>::max();

    // A band entry well BEHIND the bot's progress cursor is a door on the
    // already-walked stretch (opened and passed, or a stale flag) — NOT a
    // blocker ahead. Without this, max(0, behind) clamped it to "at door,
    // 0yd", which instantly parked the run and even fired Use() at a door the
    // bot was nowhere near. The slack covers parking inside the hitting
    // segment itself (cursor legitimately runs a few yards past the entry
    // while standing at the doorway).
    constexpr float BEHIND_SLACK = 15.0f;
    if (doorAccum + BEHIND_SLACK < cursorAccum)
        return std::numeric_limits<float>::max();

    return std::max(0.0f, doorAccum - cursorAccum);
}
bool DcEngageGeometry::IsReachable(Player* bot, float x, float y, float z)
{
    // Delegate to the chunked pathfinder. Strict PATHFIND_NORMAL was too
    // restrictive — most dungeon boss-rooms exceed PathGenerator's ~296yd
    // single-call cap, so the strict check returned false for every
    // medium-distance boss and the tank stalled with "no navigable route"
    // before getting a chance to walk the first chunk.
    //
    // The chunked check accepts a path with any forward progress and lets
    // Advance walk the remaining chunks on subsequent ticks. The position-
    // based stuck detector still catches truly unreachable destinations.
    return ChunkedPathfinder::IsReachable(bot, x, y, z);
}
bool DcEngageGeometry::IsLevelReachable(Player* bot, Unit* u)
{
    if (!bot || !u)
        return false;

    // Within DC_Z_LEVEL_TOLERANCE the candidate is on the bot's own level:
    // slopes, stairs and ramps stay under it within a corridor lookahead, so
    // we trust the caller's 2D corridor/cone/LOS test and skip the probe.
    // Otherwise a genuine other-level mob falls through to the pathfinder
    // check below.
    float const dz = std::fabs(u->GetPositionZ() - bot->GetPositionZ());
    if (dz <= DC_Z_LEVEL_TOLERANCE)
        return true;

    // Different vertical level: confirm an actual ground path. A single direct
    // probe suffices — trash is always inside the corridor/cone lookahead,
    // comfortably under PathGenerator's single-call range cap.
    PathGenerator gen(bot);
    gen.CalculatePath(u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(), /*forceDest*/ false);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;

    // PathGenerator clamps an off-mesh destination to the nearest walkable
    // poly, which on layered geometry can be the bot's *own* floor directly
    // above/below the mob — yielding a bogus NORMAL path that never descends.
    // Require the route to actually end on the candidate's level.
    Movement::PointsArray const& path = gen.GetPath();
    if (path.empty())
        return false;
    G3D::Vector3 const& end = path.back();
    if (std::fabs(end.z - u->GetPositionZ()) > DC_Z_LEVEL_TOLERANCE)
        return false;

    // A path existing is not enough: a mob at the bottom of a ledge can be 15yd
    // away in a straight line while its real approach is a 70yd ramp detour.
    // Proactively engaging/pulling it from up here sends the tank on that whole
    // detour (or wedges it on the ledge lip when the run-in can't path). Treat
    // such a candidate as NOT reachable for target selection — the route brings
    // the tank to its floor eventually, where the straight distance collapses
    // and it's picked up normally. Mirrors the navigational-distance guard in
    // IsAtBossEngage. The slack term keeps legitimate short around-the-corner
    // detours alive at close range, where the pure ratio is too strict.
    float const straight = bot->GetDistance(u);
    return gen.getPathLength() <=
           std::max(straight * DC_TRASH_DETOUR_RATIO, straight + DC_TRASH_DETOUR_SLACK);
}
bool DcEngageGeometry::ComputeCorridor(Player* bot,
                                       float bx, float by, float bz,
                                       Movement::PointsArray& out)
{
    out.clear();
    if (!bot)
        return false;
    PathGenerator gen(bot);
    gen.CalculatePath(bx, by, bz);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;
    out = gen.GetPath();
    return out.size() >= 2;
}
