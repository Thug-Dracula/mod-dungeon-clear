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
    { "PullSetback",           DcType::Float, 40,  10, 100,  true  },
    { "PullCampSafeRadius",    DcType::Float, 25,  12,  60,  true  },
    { "PullMaxDrag",           DcType::Float,100,  20, 200,  true  },

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
