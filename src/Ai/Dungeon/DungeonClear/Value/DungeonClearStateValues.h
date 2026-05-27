/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARSTATEVALUES_H
#define _PLAYERBOT_DUNGEONCLEARSTATEVALUES_H

#include <string>
#include <unordered_set>

#include "ObjectGuid.h"
#include "Position.h"
#include "Value.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"

class PlayerbotAI;

class DungeonClearEnabledValue : public ManualSetValue<bool>
{
public:
    DungeonClearEnabledValue(PlayerbotAI* botAI) : ManualSetValue<bool>(botAI, false, "dungeon clear enabled") {}
};

class DungeonClearSkippedValue : public ManualSetValue<std::unordered_set<uint32>&>
{
public:
    DungeonClearSkippedValue(PlayerbotAI* botAI)
        : ManualSetValue<std::unordered_set<uint32>&>(botAI, data, "dungeon clear skipped")
    {
    }

private:
    std::unordered_set<uint32> data;
};

class DungeonClearStuckCountValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStuckCountValue(PlayerbotAI* botAI) : ManualSetValue<uint32>(botAI, 0u, "dungeon clear stuck count") {}
};

class DungeonClearLastTargetEntryValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLastTargetEntryValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear last target entry")
    {
    }
};

class DungeonClearStallReasonValue : public ManualSetValue<std::string&>
{
public:
    DungeonClearStallReasonValue(PlayerbotAI* botAI)
        : ManualSetValue<std::string&>(botAI, data, "dungeon clear stall reason")
    {
    }

private:
    std::string data;
};

class DungeonClearLastSaidReasonValue : public ManualSetValue<std::string&>
{
public:
    DungeonClearLastSaidReasonValue(PlayerbotAI* botAI)
        : ManualSetValue<std::string&>(botAI, data, "dungeon clear last said reason")
    {
    }

private:
    std::string data;
};

class DungeonClearFallbackTargetValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearFallbackTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear fallback target")
    {
    }
};

// Sampled position from the previous DungeonClearAdvanceAction tick. Used
// alongside `dungeon clear stuck ticks` to detect geometric stuck-ness —
// the bot's MoveTo keeps returning true but its actual world position
// isn't changing. Default (0,0,0) is the "not yet sampled" sentinel since
// no real dungeon map has a (0,0,0) walkable point.
class DungeonClearLastPositionValue : public ManualSetValue<Position&>
{
public:
    DungeonClearLastPositionValue(PlayerbotAI* botAI)
        : ManualSetValue<Position&>(botAI, data, "dungeon clear last position")
    {
    }

private:
    Position data;
};

class DungeonClearStuckTicksValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStuckTicksValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear stuck ticks")
    {
    }
};

// Suppresses repeated pull-spell casts at the same engagement. Set when
// EngageDirect fires a class pull spell on a target; cleared when the
// target changes or the bot leaves combat.
class DungeonClearLastPullTargetValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearLastPullTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear last pull target")
    {
    }
};

// Sticky target for the blocking-trash engage action. Without this the
// per-tick re-scan in DungeonClearEngageTrashAction will flip between two
// roughly-equidistant corridor mobs as the bot moves, leaving it bouncing
// in place issuing MoveTo to each in turn and never reaching either. The
// stored GUID is kept across ticks as long as the unit is still alive,
// hostile, and on the bot's map; only invalidated when the unit dies,
// despawns, or the strategy itself is reset (dc on/off, party death, etc.).
class DungeonClearEngageTrashTargetValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearEngageTrashTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear engage trash target")
    {
    }
};

// GUID of the tank this (non-tank) bot is currently follow-following during
// dungeon clear. Set by DungeonClearFollowTankAction each time it issues the
// continuous MoveFollow; reset to empty once the follow has been torn down.
// Needed because MoveFollow is a persistent MotionMaster generator: when the
// DC tank goes away (dc off / death / all-cleared), the follow-tank trigger
// stops being selected and nothing would otherwise cancel the leftover
// "chase the tank" order. A self-bot can't self-heal that the way a normal
// bot does (its ordinary follow targets itself and no-ops without clearing),
// so it stays glued to the tank. This GUID lets the trigger fire one final
// teardown tick to Clear() the generator. Empty = not currently following a
// DC tank, nothing to tear down.
class DungeonClearFollowedTankValue : public ManualSetValue<ObjectGuid>
{
public:
    DungeonClearFollowedTankValue(PlayerbotAI* botAI)
        : ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "dungeon clear followed tank")
    {
    }
};

// Boss entry NextDungeonBossValue committed to on its previous computation
// (0 = none). Read back as a commit-and-hold anchor: the value keeps returning
// the same boss until it leaves the candidate set (killed, skipped, or
// despawned), with no per-tick distance re-ranking or reachability re-probe.
// Without it the nearest-boss pick flip-flops between two roughly-equidistant
// bosses (branching routes, two wings) as the bot moves and the bounded
// reachability probe flickers, wedging the bot between competing long-path
// routes — the same jitter DungeonClearEngageTrashTargetValue solves for trash.
// Self-correcting — a stale entry from a prior run is simply ignored once it's
// no longer among the live/unspawned candidates.
class DungeonClearStickyBossValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStickyBossValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear sticky boss")
    {
    }
};

// Boss entry representing a manually selected boss override.
// 0 means no override active; normal automatic progression runs.
class DungeonClearSelectedBossValue : public ManualSetValue<uint32>
{
public:
    DungeonClearSelectedBossValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear selected boss")
    {
    }
};


// Boss entry that the cached `dungeon clear long path` was built for.
// 0 means "no cached path" — Advance/triggers rebuild on next tick.
// Boss-change, dc-skip, and dc-on all clear this to force a fresh build.
class DungeonClearLongPathTargetValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLongPathTargetValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear long path target")
    {
    }
};

// Millisecond timestamp at which the cached LongPath expires. The path
// itself doesn't change as the world advances (boss positions are static)
// but the cache stays bounded so a stale path doesn't survive past edge
// cases like map reloads. Default 0 → "not cached yet".
class DungeonClearLongPathExpiresValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLongPathExpiresValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear long path expires")
    {
    }
};

// Index of the segment in the cached LongPath the bot is currently
// walking toward. Reset to 0 whenever the path target changes.
// Stored separately from the path so the segment list itself stays
// immutable while caching.
class DungeonClearCurrentHopValue : public ManualSetValue<uint32>
{
public:
    DungeonClearCurrentHopValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear current hop")
    {
    }
};

// Consecutive forward-recovery rebuild attempts. Incremented when Advance
// invalidates the long-path cache because the bot stopped making forward
// progress; reset to 0 the moment movement resumes. After several
// consecutive attempts without progress Advance escalates to a navmesh
// nudge — same FARFROMPOLY recovery the off-mesh case uses.
class DungeonClearStrideRebuildAttemptsValue : public ManualSetValue<uint32>
{
public:
    DungeonClearStrideRebuildAttemptsValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear stride rebuild attempts")
    {
    }
};

// Millisecond timestamp at which Advance first began yielding for an
// in-progress loot pickup (0 = not currently loot-yielding). Drives the
// commit-timeout: Advance yields to let the loot system pick up a corpse,
// but only for DC_LOOT_YIELD_TIMEOUT_MS; past that it force-advances past
// a corpse it can't finish looting (group-loot rolls pending, bags full)
// so the tank never parks on it forever. Reset to 0 the moment the loot
// flags clear (corpse looted, or walked out of lootDistance).
class DungeonClearLootYieldStartValue : public ManualSetValue<uint32>
{
public:
    DungeonClearLootYieldStartValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear loot yield start")
    {
    }
};

// Consecutive Advance ticks on which the long-path completed (cursor reached
// the polyline end) while the bot was still outside engage range of the boss.
// This is the "route dead-ends short of the boss" wedge: the navmesh can't get
// within DC_ENGAGE_RANGE (boss on a ledge / across a gap, or a wall-screened
// route that can't close the last yards), so NextHop reports done every tick
// and Advance rebuilds an identical 0-point path forever. The bot isn't moving
// in that loop, so the position-based stuck counter never catches it. Advance
// counts these ticks, makes a few straight-line final-approach attempts, then
// escalates to a stall. Reset on boss change and once back inside engage range.
class DungeonClearDoneNotEngagedTicksValue : public ManualSetValue<uint32>
{
public:
    DungeonClearDoneNotEngagedTicksValue(PlayerbotAI* botAI)
        : ManualSetValue<uint32>(botAI, 0u, "dungeon clear done-not-engaged ticks")
    {
    }
};

// Cursor into the cached long-path's flattened polyline plus the
// off-path tick counter. Reset whenever the path is rebuilt (boss
// change, TTL expiry, forced rebuild). The follower keeps this in sync
// each Advance tick; nothing else writes to it.
class DungeonClearFollowerStateValue : public ManualSetValue<DungeonFollowerState&>
{
public:
    DungeonClearFollowerStateValue(PlayerbotAI* botAI)
        : ManualSetValue<DungeonFollowerState&>(botAI, data, "dungeon clear follower state")
    {
    }

    void Reset() override { data = DungeonFollowerState{}; }

private:
    DungeonFollowerState data;
};

#endif
