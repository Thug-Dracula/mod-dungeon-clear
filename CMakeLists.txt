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
# Action/BetterLootRollAction.{h,cpp} (self-bot no-auto-roll override),
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
# Data/RoomAggroRegistry.{h,cpp}
#   (room-wide-aggro boss table: bosses that force the whole room into combat on
#    engage; the tank clears the room before the pull — ClearRoomBeforeBoss)
# Value/DungeonClearRoomTrashValue.{h,cpp}
#   ("dungeon clear room trash remaining" set + the no-progress give-up valve)
# Data/DungeonEventRegistry.{h,cpp} + Util/DungeonEventExecutor.{h,cpp}
#   (declarative dungeon-events framework: typed step list + state-machine
#    executor; milestone 1 anchored events, milestone 2 Gossip + conditional)
# Data/EventConditionRegistry.{h,cpp}
#   (milestone 2: conditionId predicate table for off-path Conditional events —
#    e.g. SFK "courtyard door shut & prisoner alive"; mirrors ObjectiveHookRegistry)
# Data/Events/DungeonEventTables.h + Data/Events/*.cpp
#   (the event ROWS + activation conditions split one-file-per-dungeon:
#    SunkenTemple / ZulFarrak / ShadowfangKeep / ScarletMonastery /
#    RazorfenDowns / Stratholme Events.cpp + SharedConditions.cpp. Stratholme =
#    the dead-side "Baron run": 3 conditional ziggurat acolyte clears (conditions
#    5/6/7) + a persistent Slaughterhouse chain (abominations -> Ramstein -> 2
#    add waves -> Baron). DungeonEventRegistry.cpp /
#    EventConditionRegistry.cpp keep only the builder/lookup machinery and call
#    each file's Register<Dungeon>Events/Conditions appender — explicit calls so
#    every per-dungeon TU stays linked into the module static lib. Add a dungeon
#    = one new file + one call.)
#   (fable2 T2.3: the per-dungeon file is now the dungeon's whole "definition
#    unit" — its BossRosterPatch (Register<Dungeon>Roster, aggregated by
#    BossRosterRegistry) and any wing layout (Register<Dungeon>Wings, aggregated
#    by DungeonWingRegistry) moved in alongside the event rows, built with the
#    shared DcRoster helpers in Data/Events/DungeonRosterBuilders.h. NEW file:
#    Data/Events/MaraudonEvents.cpp — wings-only definition unit for map 349.)
# Action/DcActionShared.{h,cpp}, Action/DcAdvanceAction.cpp,
# Action/DcEngageActions.cpp, Action/DcPullActions.cpp,
# Action/DcFollowerActions.cpp
#   (nav/pull review Phase C Part 2 — the 4,608-line Action/DungeonClearActions.cpp
#    split along its four family seams: advance navigation / engage family /
#    pull FSM + maneuver / follower behaviors, with the cross-family
#    anon-namespace helpers + tuning constants promoted into namespace
#    DcActionShared (DcActionShared.{h,cpp}). DungeonClearActions.cpp is REMOVED;
#    DungeonClearActions.h stays as the umbrella class-declaration header.)
# Util/DcDecisionJson.h
#   (headless-sim Tier 1: shared flat-JSONL writer/parser primitives for the
#    decision capture/replay IO — header-only, used by the pull-decision IO)
# Util/DcPullDecision.{h,cpp} + Util/DcPullDecisionIo.{h,cpp}
#   (headless-sim Tier 1: pure Dynamic-pull governor decision core
#    DecidePull(PullObservation) -> PullVerdict, extracted from
#    DcPullPlanner::UpdateDynamicPullMode + its JSONL capture/replay IO, gated by
#    DcSettings RecordDecisions. Replayed offline by t/replay_pull against
#    t/fixtures/pull/*.jsonl; edge-pinned by t/TestPullDecisions; driven through
#    a scripted pull by t/TestScenarioDriver. Tier 2 nav harness (t/NavHarness,
#    t/TestNavGeometry, tools/slice_mapdata.py) is test-only, not GLOBed here.)
# Value/DungeonClearHealTargetValue.{h,cpp}
#   (LOS-blind, tank-biased most-hurt heal target — feeds the healer LOS
#    reposition fix: DungeonClearHealReposition{Trigger,Action})
# Data/Events/DireMaulEvents.cpp
#   (Dire Maul East: Ironbark the Redeemed anchored objective/event — gossip him
#    to open the Conservatory Door to Alzzin; ordered just before Alzzin via the
#    BossRosterRegistry map-429 patch + East-wing objective entry)
# src/DcStrategyGate.{h,cpp}
#   (dungeon-gated strategy install: the DC strategies are installed on a bot iff
#    it is on a dungeon/raid map — Map::IsDungeon() — instead of on every bot in
#    the realm. Removes the realm-wide DC trigger-ladder idle cost. Driven by
#    OnPlayerLogin + OnPlayerMapChanged (responsive) and a throttled world-tick
#    sweep in DungeonClearReaperScript (correctness net across ResetStrategies).
#    Pure decision kernel DcStrategyGate::Decide is headless-tested by
#    t/TestStrategyGate.cpp.)
# Data/DcNavPenaltyRegistry.{h,cpp} + Util/DcRouteFilter.{h,cpp}
#   (no-go route volumes: DcRouteFilter extends dtQueryFilterExt with a per-map
#    hand-authored no-go AABB table (DcNavPenaltyRegistry), so the long-range A*
#    corridor avoids navmesh shortcuts a real player can't follow — e.g. the LBRS
#    chasm wall-climb. Wired into LongRangePathfinder::BuildCoreFromMesh (now takes
#    a mapId) and screened in the StridedPathfinder fallback too.)
# Action/DcHakkar.cpp (Sunken Temple Avatar of Hakkar orchestration split out of
#   DcEngageActions.cpp — fable2 T2.5)
# REMOVED Data/EventConditionRegistry.{h,cpp}: conditional-event activation is now
#   a function pointer on DungeonEvent (.Conditional(&Predicate)), so the global
#   condition-id registry + id ledger are gone (fable2 T2.3 / events F7)
# Util/DcRegroupDecision.{h,cpp}
#   (combat-regroup Option B: pure contribution-gate kernel DecideCombatRegroup(
#    RegroupInputs) -> RegroupVerdict — "reconnect only when a follower truly can't
#    contribute". Engine-free, edge-pinned by t/TestCombatRegroup. Drives the
#    rewritten DungeonClearRegroupCombat{Trigger,Action}.)
# Data/Events/GnomereganEvents.cpp
#   (Gnomeregan, map 90: conditional UseGO events for the Workshop Door 90858
#    and the Final Chamber 142207 — unlocks progression to Grubbis
#    and Mekgineer Thermaplugg respectively.)
# Data/Events/MaraudonEvents.cpp (now also has events)
#   (Maraudon, map 349: added conditional UseGO event for the Portal to Inner
#    Maraudon 178404 — teleports the party from the Celbras seal area to
#    Pristine Waters after Celebras is dead, replacing the earlier wings-only.)
# Data/Events/OldHillsbradEvents.cpp
#   (Escape from Durnholde, map 560: Brazen ride -> bomb 5 barrels + Lt. Drake ->
#    ONE EscortCreature step freeing Thrall and assisting him through Skarloc, the
#    mounted Tarren Mill ride, the barn/church/inn waves and Epoch Hunter, gated on
#    the instance progress counter. New step kind UseItemOnGO (cast a quest-item
#    spell AT a positioned GO — the barrel-bomb SPELLHIT); EscortCreature gains an
#    instance-data completion gate, a gossip-flag resume, and party mount speed-match.)
# (this comment bumps mtime to force CMake re-glob on next build)
