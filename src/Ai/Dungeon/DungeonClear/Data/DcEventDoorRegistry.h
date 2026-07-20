/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCEVENTDOORREGISTRY_H
#define _PLAYERBOT_DCEVENTDOORREGISTRY_H

#include "Common.h"

// Per-ENTRY list of door gameobjects that are SCRIPT-ONLY: the live client
// refuses a direct player open and ONLY an in-game event opens them, even though
// their template (an empty lock-85, the same template as plenty of plainly
// clickable doors) reads as openable to BotCanOpenDoorLikePlayer / DcDoorPolicy.
// A bot generic-Use()ing one of these toggles the server GO state while the
// client still treats the door as shut — a desync — and it also skips the
// intended event (e.g. Shadowfang Keep's courtyard door, which only opens when a
// freed prisoner walks over and unlocks it).
//
// This is DELIBERATELY keyed by GO ENTRY, not by lock id: lock 85 is shared with
// many doors bots SHOULD open (Deadmines Factory/Foundry/Mast Room, etc.), so a
// lock-level rule would break them. Keep this list to doors verified to be
// script/event-opened only; the door-blocked action consults it before deciding
// it is "entitled" to open a door, and leaves a listed door for the events
// framework or the human instead.
namespace DcEventDoorRegistry
{
    inline bool IsScriptOnly(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 18895:  // Shadowfang Keep — Courtyard Door (freed-prisoner event)
                return true;
            default:
                return false;
        }
    }

    // Doors NAVIGATION must ignore entirely: never flagged as a corridor
    // blocker, never opened, never a reason to park or auto-pause. These are
    // interact-THROUGH gates — the run's objective is completed from the
    // players' side of the shut door (a gossip through the bars), after which
    // the event script opens the door itself. Flagging one as blocking is
    // always wrong: the route intentionally ends beside it, and the pause
    // machinery would halt a run that needs nothing from the door at all.
    inline bool IsNavigationIgnored(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 184393:  // Old Hillsbrad — Thrall's Prison Door (gossip through
                          // the gate; his script opens it via EVENT_OPEN_DOORS)
                return true;
            default:
                return false;
        }
    }

    // The MIRROR-IMAGE special case: door gameobjects carrying NO lock at all
    // (template lockId 0) that a player nonetheless opens by simply clicking
    // them — ordinary traversal gates the dungeon expects you to walk through.
    //
    // BotCanOpenDoorLikePlayer otherwise refuses every lock-free door, because
    // lockId 0 is ALSO the shape of script/event seals the bot must not pop
    // (Uldaman's Seal of Khaz'Mul, lock-free and only opened by the keystone
    // event, isn't flagged GO_FLAG_NOT_SELECTABLE until its encounter is done,
    // so the generic flag screen can't tell them apart). We can't relax the
    // lock-free rule wholesale; instead we allowlist the entries verified in
    // the world DB to be plain clickable doors — no ScriptName, no AIName, no
    // instance-script GO-state control, no SmartAI.
    //
    // Scholomance's Iron Gates (175611-175618, 175620) and plain interior Doors
    // (175610, 175619) are exactly this: lock-free, scriptless room-to-room
    // gates the player clicks open. (The dungeon's *event* gates — Kirtonos
    // 175570 and the seven Gandling gates 177371-177377 — are deliberately
    // EXCLUDED; the instance script drives their state.)
    inline bool IsLockFreeClickable(uint32 goEntry)
    {
        switch (goEntry)
        {
            // Scholomance — interior traversal gates/doors (map 289)
            case 175610:  // Door
            case 175611:  // Iron Gate
            case 175612:  // Iron Gate
            case 175613:  // Iron Gate
            case 175614:  // Iron Gate
            case 175615:  // Iron Gate
            case 175616:  // Iron Gate
            case 175617:  // Iron Gate
            case 175618:  // Iron Gate
            case 175619:  // Door
            case 175620:  // Iron Gate
            // Shadowfang Keep — interior traversal doors (map 33)
            case 18972:   // Sorcerer's Gate (between Fenrus and Arugal)
            // Scholomance — interior traversal doors (map 289)
            case 175968:  // Hoard Door
            // Stratholme — interior traversal doors (map 329)
            case 175967:  // The Bastion Door (live side, Dathrohan's chamber)
            // Scarlet Monastery — interior traversal doors (map 189)
            case 101850:  // Cathedral Door
            case 101851:  // Armory Door
            case 101854:  // Herod's Door
            case 19835:   // Great Hall Doors
            case 104591:  // Chapel Door (Mograine/Whitemane room)
                return true;
            default:
                return false;
        }
    }

    // Doors that are lock-free but have SmartAI or other flags preventing Use()
    // from working. When the bot is entitled to open via IsLockFreeClickable but
    // normal Use() fails, force-open via SetGoState as a fallback.
    inline bool IsForceOpenDoor(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 175368:  // Stratholme — Service Entrance Gate (lockId=0,
                          // SmartGameObjectAI with no scripts intercepts Use)
            case 175967:  // Stratholme — Bastion Door (lockId=0, flags=34
                          // GO_FLAG_LOCKED prevents Use() from opening)
            case 175968:  // Scholomance — Hoard Door (lockId=0, flags=34)
            case 18971:   // Shadowfang Keep — Arugal's Lair (lockId=85,
                          // event door, empty lock template)
            case 18972:   // Shadowfang Keep — Sorcerer's Gate (lockId=0)
            case 90858:   // Gnomeregan — Workshop Door (lockId=92, needs key)
            case 142207:  // Gnomeregan — Final Chamber Door (lockId=86, needs key)
                return true;
            default:
                return false;
        }
    }
}

#endif
