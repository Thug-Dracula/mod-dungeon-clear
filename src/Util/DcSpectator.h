/*
 * mod-dungeon-clear — DcSpectator.h
 *
 * Spectator free-camera mode (`.dc spectate` / addon "spectate"). Detaches the
 * human player into a free-flying camera while their character keeps running
 * under bot AI (bot-self mode). Mechanism: possess an invisible, unattackable,
 * flying WORLD_TRIGGER TempSummon via Unit::SetCharmedBy(CHARM_TYPE_POSSESS) —
 * the Eye of Acherus pattern. The client natively moves its camera and input
 * to the possessed unit, and the server streams visibility around the new
 * seer, so the camera can fly anywhere in the loaded map.
 *
 * This is session plumbing, not bot AI: it acts on the issuing player directly
 * and deliberately does NOT go through DungeonClearDispatch, the action/
 * trigger/strategy pipeline, or the leader-tank concept. It is independent of
 * the DC run — `dc off`/pause do not end spectate.
 *
 * Threading: Start/Toggle run on the world thread (commands, the addon chat
 * hook), but the teardown hooks (death, teleport) fire on map threads, and the
 * AI mover window below runs on every map thread each player tick — so the
 * internal state map is mutex-guarded, with a lock-free active-count fast path
 * for the per-tick window. Per-player operations never race each other (a
 * player's session phase and map update are sequential); the lock only guards
 * the shared container across players on different maps.
 */

#ifndef _DUNGEON_CLEAR_DC_SPECTATOR_H
#define _DUNGEON_CLEAR_DC_SPECTATOR_H

#include <string>

class Player;

namespace DcSpectator
{
    // True while `player` has an active spectator camera (guid-keyed lookup).
    bool IsActive(Player* player);

    // Summon + possess the camera dummy. Returns false and fills `whyNot`
    // (when non-null) if the player is in a state that refuses possession.
    bool Start(Player* player, std::string* whyNot = nullptr);

    // Tear down the possession and despawn the dummy. Idempotent — safe to
    // call blind from every exit path (teleport, death, logout, toggle).
    void Stop(Player* player);

    // Stop if active, Start otherwise. Returns false only on a refused Start.
    bool Toggle(Player* player, std::string* whyNot = nullptr);

    // Scoped "AI mover window". While spectating, the session's m_mover is the
    // camera dummy, and the core packet handlers playerbots injects bot actions
    // into (HandleUseItemOpcode, HandleGameObjectUseOpcode, …) silently drop on
    // their remote-control guard `m_mover != _player` — so a self-bot could not
    // use items (soulstone, healthstone, potions, food) while its human flew
    // the camera. These two calls bracket the bot's AI tick (both fired from
    // PLAYERHOOK_ON_AFTER_UPDATE on the player's map thread — see the mover
    // window scripts in DungeonClearModule.cpp for the ordering contract) and
    // swap m_mover back to the player for just that duration. Client movement
    // packets for the dummy are processed in the session phases, never inside
    // this window, so camera control is unaffected. Both are cheap no-ops
    // while nobody on the server is spectating.
    void BeginBotAiMoverWindow(Player* player);
    void EndBotAiMoverWindow(Player* player);
}

#endif
