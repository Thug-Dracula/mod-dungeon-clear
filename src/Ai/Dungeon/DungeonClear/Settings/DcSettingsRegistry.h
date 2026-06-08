/*
 * mod-dungeon-clear — DcSettingsRegistry.h
 *
 * The single source of truth for every DungeonClear tunable. Each row pairs a
 * config key with its type, default, clamp range, and whether players may
 * override it from the companion addon. Everything else — the conf default
 * lookup, the validation/clamping of client-supplied overrides, and (if the
 * addon panel is schema-driven) the UI controls — derives from this table.
 *
 * Adding a new option later is a one-line change here plus the matching line in
 * mod_dungeon_clear.conf.dist; read it at the use site via DcSettings::GetT().
 *
 * The `key` is the suffix only — the "DungeonClear." prefix is added by the
 * accessor when it falls back to sConfigMgr.
 */

#ifndef _DUNGEON_CLEAR_DC_SETTINGS_REGISTRY_H
#define _DUNGEON_CLEAR_DC_SETTINGS_REGISTRY_H

#include <cstddef>
#include <string_view>

enum class DcType
{
    Bool,
    UInt,
    Int,
    Float
};

struct DcSettingDef
{
    char const* key;          // config key suffix, e.g. "BossEngageRangeFloor"
    DcType      type;
    double      defVal;       // fallback when the conf line is absent
    double      minVal;       // clamp floor for client-supplied overrides
    double      maxVal;       // clamp ceiling
    bool        playerFacing; // exposed to the addon UI + accepts overrides?
};

// The registry. Player-facing rows are overridable per dungeon run; server-only
// rows live here purely so their default is defined in one place (the accessor
// rejects overrides for them and the addon hides them).
inline constexpr DcSettingDef kDcSettings[] =
{
    { "LootMinQuality",        DcType::UInt,   0,   0,   6,  true  },
    { "IgnoreChests",          DcType::Bool,   1,   0,   1,  true  },
    { "RestHealthPct",         DcType::UInt,   0,   0, 100,  true  },
    { "RestManaPct",           DcType::UInt,   0,   0, 100,  true  },
    { "PreventBotRelease",     DcType::Bool,   1,   0,   1,  true  },
    { "PartyMaxSpread",        DcType::Float, 25,  10,  60,  true  },

    // In-combat regroup: keep followers grouped on the leader tank DURING a fight,
    // not just on the route. Once the leash loosens (advanced/dynamic pull) a fight
    // can drift a follower out of the tank's line of sight or far behind, and stock
    // combat won't pull it back — a healer with no LOS to the tank just stands there
    // and the party dies. CombatRegroup is the master toggle; CombatRegroupDistance
    // is the max leash beyond which ANY follower closes back on the tank (a healer
    // also closes whenever it loses LOS to the tank, inside that distance or not).
    // See DungeonClearRegroupCombat{Trigger,Action}.
    { "CombatRegroup",         DcType::Bool,   1,   0,   1,  true  },
    { "CombatRegroupDistance", DcType::Float, 25,  10,  60,  true  },
    { "BossEngageRangeFloor",  DcType::Float, 12,   5,  40,  true  },
    { "BossEngageRangeCap",    DcType::Float, 30,  10,  60,  true  },
    { "TrashWidthFloor",       DcType::Float,  8,   4,  30,  true  },
    { "TrashWidthCap",         DcType::Float, 30,  10,  60,  true  },
    { "DynamicAggroRange",     DcType::Bool,   1,   0,   1,  true  },
    { "AggroRangeMargin",      DcType::Float,  2,   0,  10,  true  },

    // Advanced pull (LOS pull-to-camp). Setback is how far BACK along the cleared
    // route the camp is placed (and therefore how far the tank drags the pack) —
    // dungeon mobs have no leash, so this is purely "how much room the party
    // gets". SafeRadius is the clearance the camp keeps from any OTHER pack so the
    // fight can't aggro a neighbour; if the setback point isn't clear the placer
    // walks further back (up to MaxDrag) until it is. See ComputeSafeCamp.
    { "PullSetback",           DcType::Float, 25,  10, 100,  true  },
    { "PullCampSafeRadius",    DcType::Float, 25,  12,  60,  true  },
    { "PullMaxDrag",           DcType::Float, 40,  20, 200,  true  },

    // Seconds the party stays passive AFTER the leader commits the pull (flips to
    // Engage) before DPS are freed to fight — gives the tank a threat head start.
    // Only the graceful Engage commit is delayed; ending/pausing the run or the
    // camp-safety valve release at once. 0 = release the party immediately.
    { "PullPlayerReleaseDelay", DcType::Float, 1.5,  0,  10,  true  },

    // Seconds a follower's pet stays passive AFTER its owner is released (on top
    // of PullPlayerReleaseDelay). Releasing pet and owner in lockstep lets the
    // pet charge in and pull aggro off the tank before he's settled, botching the
    // pull; the delay lets the tank establish threat first. 0 = release at once.
    { "PullPetReleaseDelay",   DcType::Float, 2.5,  0,  10,  true  },

    // PullCommitRange{Floor,Cap}: how close the pack must be before the tank stops,
    // holds, and waits for the party at camp BEFORE stepping in to tag. Sized to the
    // pack's REAL aggro radius (Creature::GetAggroRange + reaches + AggroRangeMargin
    // — the same exact core value the boss handoff uses) so the tank Forms just
    // OUTSIDE aggro instead of face-pulling mid-glide. Clamped to [floor,cap]; the
    // cap stays inside the ~35yd pull-detection band. Honoured only while
    // DynamicAggroRange = 1; otherwise the fixed fallback applies.
    { "PullCommitRangeFloor",  DcType::Float, 16,   5,  40,  true  },
    { "PullCommitRangeCap",    DcType::Float, 34,  10,  60,  true  },

    // Dynamic pull (setting 2): the tank auto-picks Leeroy vs Advanced per pack.
    // ChainRadius is how close ANOTHER pack must be to the target pack before the
    // pull is treated as a multi-pack room and handled with the careful Advanced
    // maneuver; below it (an isolated pack) the tank just Leeroys it. The effective
    // chain distance is floored at PullCampSafeRadius (the clearance the Advanced
    // camp keeps from other packs) so the decision can never pick a Leeroy fight
    // closer to a neighbour than the careful pull would itself tolerate — raising
    // this above PullCampSafeRadius makes Dynamic more cautious still; lowering it
    // has no effect below that floor. The decision and the clearance test are also
    // height-aware (a pack a floor above/below never counts). LargePackThreshold
    // forces Advanced for a single big pack even with no neighbour. See
    // DungeonClearUtil::ClassifyPullAdvanced.
    { "PullDynamicChainRadius",     DcType::Float, 28,  5,  40,  true  },
    { "PullDynamicLargePackThreshold", DcType::UInt, 5,  1,  20,  true  },

    // Dynamic pull only: how far BACK the party trails the tank while it scouts
    // toward the next pack and sizes up the Leeroy/Advanced verdict (leader out of
    // combat, pull phase Idle). The normal ~6yd follow bubble would trail the party
    // right onto the tank's heels and into the pack's aggro arc before the tank had
    // committed, accidentally triggering the pull. This wider lag keeps the party a
    // safe distance back so the tank reaches aggro range alone, decides, and only
    // then does the party arrive (it holds at camp for Advanced, or catches up to
    // charge once the tank commits the Leeroy). See DungeonClearFollowTankAction.
    { "PullDynamicPartyLag",   DcType::Float, 15,   6,  40,  true  },

    // Liquid avoidance. The route producers include water/magma polys so the
    // bot CAN swim/wade when there is no dry alternative, but with these per-area
    // Detour cost multipliers a crossing only wins when it is genuinely shorter:
    // an all-land detour up to WaterPathCost times longer than the water shortcut
    // is preferred. 1.0 = no preference (water as cheap as land). MagmaPathCost is
    // set high so lava is shunned but still traversable as an absolute last
    // resort (the player nav-filter already excludes slime outright). These feed
    // dtQueryFilter::setAreaCost in LongRangePathfinder + CorridorCenter; both run
    // off the map thread, so they are server-only (read straight from conf, never
    // the per-run override store). See DungeonClearGeometry::ApplyLiquidAreaCosts.
    { "WaterPathCost",         DcType::Float,  3,   1,  50,  false },
    { "MagmaPathCost",         DcType::Float, 20,   1, 1000, false },

    // Server-only (not overridable from the addon).
    { "AsyncPathfinding",      DcType::Bool,   1,   0,   1,  false },
    { "PathCenterEnable",      DcType::Bool,   1,   0,   1,  false },
    { "PathWallClearance",     DcType::Float,  3,   0,  10,  false },
    { "PathCenterMaxPush",     DcType::Float,  3,   0,  10,  false },
    { "PathCenterSmoothIters", DcType::Int,    2,   0,   8,  false },
};

inline constexpr std::size_t kDcSettingCount =
    sizeof(kDcSettings) / sizeof(kDcSettings[0]);

// Linear lookup by key suffix; nullptr if the key is not registered. The table
// is tiny, so a scan is cheaper than any map and keeps it constexpr-friendly.
inline DcSettingDef const* FindDcSetting(std::string_view key)
{
    for (DcSettingDef const& d : kDcSettings)
        if (key == d.key)
            return &d;
    return nullptr;
}

#endif
