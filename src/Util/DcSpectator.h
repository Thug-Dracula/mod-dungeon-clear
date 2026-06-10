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
 * World-thread only (commands, the addon chat hook, and PlayerScript hooks all
 * run there) — the internal state map is unsynchronized by design.
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
}

#endif
