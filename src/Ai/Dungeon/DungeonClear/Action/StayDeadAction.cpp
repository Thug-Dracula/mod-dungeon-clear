/*
 * mod-dungeon-clear — StayDeadAction.cpp
 */

#include "StayDeadAction.h"

#include "Config.h"

bool DungeonClearStayDeadAction::isUseful()
{
    // Read live (not cached) so `.reload config` can toggle the behaviour without
    // a restart. The dead-state "auto release" trigger fires on the throttled
    // "often" cadence, so the per-call config lookup is negligible.
    if (sConfigMgr->GetOption<bool>("DungeonClear.PreventBotRelease", true))
        return false;  // never auto-release; bot stays a corpse until rezzed

    return AutoReleaseSpiritAction::isUseful();
}
