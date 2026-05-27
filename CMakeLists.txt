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

# New files added: DungeonClearDispatch.h, DungeonClearAddonHook.cpp
# (this comment bumps mtime to force CMake re-glob on next build)
