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

    // Diagnostics: record every boss-approach decision (the pure DecideApproach
    // observation + verdict) to a JSONL capture file for the offline replay
    // harness. OFF by default — turn it on only to freeze a live freeze/stutter
    // into a permanent regression fixture (see t/replay_decisions.cpp). The
    // capture path is dungeonclear_decisions.jsonl in the worldserver's working
    // dir, overridable via the DUNGEONCLEAR_DECISIONS_FILE env var.
    { "RecordDecisions",       DcType::Bool,   0,   0,   1,  true  },
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
    { "CombatRegroupDistance", DcType::Float, 17,  10,  60,  true  },
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
    { "PullMaxDrag",           DcType::Float, 35,  20, 200,  true  },

    // Ranged LOS-break pull. When the pulled pack has a ranged attacker (caster,
    // archer, wand — see DcEngageGeometry::IsRangedAttacker) it would otherwise
    // stand at the room's edge and plink the party across open ground. With
    // PullRangedLosBreak on, ComputeSafeCamp keeps walking the camp BACK along the
    // cleared route until it finds a point with no line of sight to the pack —
    // typically the doorway/corner the tank entered through — so the rangers are
    // forced to close to melee at camp. PullRangedMaxDrag is the (larger) drag cap
    // used only for these pulls, since the corner can sit well beyond the normal
    // PullMaxDrag; if no out-of-sight point is reachable within it the placer falls
    // back to the farthest cleared point (best effort — LOS can't always be broken).
    // PullRangedSpellRangeFloor is the spell max-range above which a damaging
    // creature spell counts as "fights at range" (server-only tuning detail).
    { "PullRangedLosBreak",        DcType::Bool,   1,   0,   1,  true  },
    { "PullRangedMaxDrag",         DcType::Float, 60,  20, 250,  true  },
    { "PullRangedSpellRangeFloor", DcType::Float, 15,   8,  40,  false },

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

    // CC-assist: when the leader tank is CC'd mid-pull while dragging the pack to
    // camp (stunned / feared / confused / rooted, or slowed below PullCcSlowFloor
    // of base run speed), the drag fails — the tank can't retreat and just eats
    // the pack while the party stands passive at camp. PullCcAssist (master
    // toggle) aborts that pull the instant the CC has lasted PullCcAssistGrace
    // seconds, dropping the party out of its passive hold to pile onto the pack and
    // help (via the existing camp-fight assist). The grace ignores a brief micro-CC
    // so a 0.5s stutter-stun doesn't throw an otherwise-fine pull away; sustained
    // CC (the pull IS failing) releases the party. Daze is already immunized for
    // the pull, so a slow detected here is a real debuff (Hamstring, web, frost).
    // See DungeonClearPullManeuverAction + DungeonClearMath::ShouldAbortPullForCc.
    { "PullCcAssist",          DcType::Bool,   1,   0,    1,  true  },
    { "PullCcAssistGrace",     DcType::Float, 1.0,  0,   10,  true  },
    { "PullCcSlowFloor",       DcType::Float, 0.6,  0.1,  1,  true  },

    // PullCommitRange{Floor,Cap}: how close the pack must be before the tank stops,
    // holds, and waits for the party at camp BEFORE stepping in to tag. Sized to the
    // pack's REAL aggro radius (Creature::GetAggroRange + reaches + AggroRangeMargin
    // — the same exact core value the boss handoff uses) so the tank Forms just
    // OUTSIDE aggro instead of face-pulling mid-glide. Clamped to [floor,cap]; the
    // cap stays inside the ~35yd pull-detection band. Honoured only while
    // DynamicAggroRange = 1; otherwise the fixed fallback applies.
    { "PullCommitRangeFloor",  DcType::Float, 16,   5,  40,  true  },
    { "PullCommitRangeCap",    DcType::Float, 34,  10,  60,  true  },

    // Dynamic pull (setting 2): the tank auto-picks Leeroy vs Advanced per pack by
    // ESTIMATING how many mobs aggro if it Leeroys on top of the target — proximity
    // aggro from each mob's own level-scaled aggro radius plus one CallForHelp
    // assist hop (see DcPullPlanner::ClassifyPullAdvanced and DungeonClearMath::
    // EstimateAggroCount). MaxLeeroyMobs is the party's comfortable simultaneous-
    // mob ceiling: an estimate ABOVE it => Advanced (peel one cluster at a time),
    // at/below => Leeroy. This single count is the whole verdict and self-tunes per
    // zone/level because the reach comes from the real creature aggro radius, not a
    // hand-set chain distance. (Replaces PullDynamicChainRadius +
    // PullDynamicLargePackThreshold, both removed.)
    { "PullDynamicMaxLeeroyMobs",   DcType::UInt,   5,  1,  20,  true  },
    // CombatSpread pads every proximity reach to model the party drifting to
    // flank/kite during the fight (the camp is a disc, not a point). This is a
    // zone-independent fudge for player movement, NOT a per-zone distance, so one
    // default holds everywhere; higher = counts mobs slightly farther out = more
    // cautious. (The assist-hop reach is NOT a setting — it reads the engine's own
    // CreatureFamilyAssistanceRadius directly, see ClassifyPullAdvanced.)
    { "PullCombatSpread",           DcType::Float,  6,  0,  20,  true  },

    // Dynamic pull only: how far BACK the party trails the tank while it scouts
    // toward the next pack and sizes up the Leeroy/Advanced verdict (leader out of
    // combat, pull phase Idle). The normal ~6yd follow bubble would trail the party
    // right onto the tank's heels and into the pack's aggro arc before the tank had
    // committed, accidentally triggering the pull. This wider lag keeps the party a
    // safe distance back so the tank reaches aggro range alone, decides, and only
    // then does the party arrive (it holds at camp for Advanced, or catches up to
    // charge once the tank commits the Leeroy). See DungeonClearFollowTankAction.
    { "PullDynamicPartyLag",   DcType::Float, 15,   6,  40,  true  },
    // Dynamic pull only: Leeroy roll-in. How far OUTSIDE the tank's commit range
    // (yd) the scout lag above releases when the standing verdict is Leeroy — the
    // tank is committing to the charge, so the party closes the gap DURING its
    // final approach and arrives roughly with first contact, instead of standing
    // flat-footed at the lag ring until combat registers and only then starting a
    // 15-20yd run (the 2-3s "bots watching their tank fight" beat on every Leeroy).
    // 0 = release only once the tank reaches commit range; larger = the party
    // rolls earlier alongside the tank. See DcLeaderSignal::IsLeaderDynamicScouting.
    { "PullDynamicRollInLead", DcType::Float,  8,   0,  30,  true  },

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

    // Submerged swim legs (Tier A). When the navmesh route to a target dead-ends
    // AND water lies between, the bot greedily 3D-swims to it instead of stalling
    // (the navmesh has no mesh under liquid — only a surface sheet — so a
    // submerged tunnel is otherwise unreachable). SwimMaxRange bounds how far a
    // dead-end target may be before a swim is attempted (caps the greedy search
    // and avoids trying to swim to something genuinely out of reach).
    { "SwimEnable",            DcType::Bool,   1,   0,    1,  false },
    { "SwimMaxRange",          DcType::Float, 250, 30, 1000,  false },

    // Spectator free-camera (`.dc spectate`): movement speed multiplier applied
    // to the possessed camera dummy (flight and run). See Util/DcSpectator.h.
    { "SpectateSpeed",         DcType::Float, 2.5, 0.5,  8,  true  },

    // Server-only (not overridable from the addon).
    { "AsyncPathfinding",      DcType::Bool,   1,   0,   1,  false },
    { "PathCenterEnable",      DcType::Bool,   1,   0,   1,  false },
    { "PathWallClearance",     DcType::Float,  3,   0,  10,  false },
    { "PathCenterMaxPush",     DcType::Float,  5,   0,  10,  false },
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
