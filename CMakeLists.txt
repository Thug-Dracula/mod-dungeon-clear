# mod-dungeon-clear — AzerothCore module build registration.
#
# Sources are auto-collected from src/ (recursively) by AzerothCore's module
# system; AC_ADD_SCRIPT registers the script-loader path. The module compiles
# into the shared `modules` static library alongside mod-playerbots, so its
# subclasses of the playerbots engine classes link directly.
#
# REQUIRES mod-playerbots to be present and enabled — this is a playerbots
# extension, not a standalone module.
#
# NOTE: sources are GLOBed at configure time (no CONFIGURE_DEPENDS), so adding
# or removing a .cpp requires CMake to re-run. Editing this file bumps its
# mtime, which CMake treats as a configure dependency and reconfigures on the
# next build — the reliable way to force a re-glob after the file set changes.

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod_dungeon_clear.conf.dist")

# New files added: DungeonClearDispatch.h, DungeonClearAddonHook.cpp,
# Data/DungeonWingRegistry.{h,cpp}, Action/StayDeadAction.{h,cpp},
# Util/CorridorCenter.{h,cpp}, Value/DungeonClearLiveBossValue.{h,cpp},
# Util/DcPathWorker.{h,cpp} (async pathfinding worker),
# Util/DungeonClearGeometry.{h,cpp} (shared LOS/distance primitives),
# Settings/DcSettingsRegistry.h + Settings/DcSettings.{h,cpp}
#   (per-run settings registry + override accessor)
# Value/DungeonClearPullModeCurrentValue.{h,cpp}
#   (order-independent live pull-mode / Dynamic-verdict accessor)
# Util/SwimPathfinder.{h,cpp}
#   (Tier A greedy 3D swim navigator for submerged tunnels; driven via a raw
#    3D escort spline off the new DungeonClearSwimState value)
# Util/DungeonClearTuning.h
#   (single home for shared DC_* tuning constants — constant-centralization)
# Util/DcEngageGeometry.{h,cpp}
#   (DungeonClearUtil decomposition: ranges/doors/reachability/corridor unit)
# Util/DcPartyState.{h,cpp}
#   (DungeonClearUtil decomposition: readiness/rest gate/status text unit)
# Util/DcLootPolicy.{h,cpp}
#   (DungeonClearUtil decomposition: loot quality floor/skip/give-up unit)
# Util/DcLeaderSignal.{h,cpp}
#   (DungeonClearUtil decomposition: cross-bot leader/pull broadcast unit)
# Util/DcFollowerLifecycle.{h,cpp}
#   (DungeonClearUtil decomposition: follow/passive marks + reapers; owns g_dc* statics)
# Util/DcStatusPublisher.{h,cpp}
#   (DungeonClearUtil decomposition: addon comms + event-driven status push)
# Util/DcPullPlanner.{h,cpp}
#   (DungeonClearUtil decomposition: camp placement + dynamic-pull classification)
# Util/DcTargeting.{h,cpp}
#   (DungeonClearUtil decomposition: find-the-thing-to-fight unit; owns the
#    corridor trash-scan helpers. Completes the split — DungeonClearUtil class
#    is now empty; DungeonClearUtil.h is the umbrella + DC_PULL_* macros.)
# Util/DungeonClearApproach.{h,cpp}
#   (advance phase-machine §C: pure, engine-free decision core for the boss-
#    approach tail ladder — DecideApproach(Observation) -> Verdict, gtested at
#    every threshold edge; mirrors DungeonClearAdvanceAction's tail order.)
# Util/DungeonClearApproachIo.{h,cpp}
#   (orchestration replay harness: engine-free JSONL serialize/parse of the
#    DecideApproach (observation -> verdict) record + the live capture hook,
#    gated by DcSettings RecordDecisions. Replayed offline by t/replay_decisions
#    against t/fixtures/*.jsonl to lock every historical freeze as a regression.)
# src/Util/DcSpectator.{h,cpp}
#   (module-root src/Util/ — session plumbing, not bot AI: spectator
#    free-camera via WORLD_TRIGGER possession, `.dc spectate` / addon button)
# (this comment bumps mtime to force CMake re-glob on next build)
