/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "GameObject.h"
#include "Log.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Timer.h"

// --- Dire Maul East (map 429) — Ironbark / Conservatory Door -------------
// Alzzin the Wildshaper's grove is sealed behind the Conservatory Door (GO
// 176907), which the party cannot open. Ironbark the Redeemed (14241), who
// stands at the west entrance corridor, opens it: pick his single gossip option
// (menu 5602, option 0) and his SmartAI walks him east along a 14-point path to
// the door and, ~7s after arriving, swings it open (and sets instance data
// TYPE_EAST_WING_PROGRESS=2), then despawns. Total gossip->open ≈ 20-30s.
//
// Anchored (not Conditional like Shadowfang Keep's twin courtyard door): when
// this becomes relevant the tank is ~190 yds away in the southern boss chamber
// (Zevrim/Hydrospawn/Lethtendris are reached WITHOUT the door; only Alzzin sits
// behind it). A Conditional Gossip step has no long-range navigation — it only
// HopTo's (raw MovePoint, 74-node capped) — which can't reliably cross that gap.
// An anchored OBJECTIVE gets the boss-nav LongRangePathfinder to deliver the
// tank to Ironbark first (BossRosterRegistry slots the objective at his spawn,
// ordered just before Alzzin); then this gossips him and waits for the door.
//
// No MoveTo to the door is needed: while WaitForGOState holds, the tank stays at
// Ironbark's anchor (west) and Ironbark walks east and opens the door himself.
// Holding the run here (relevance 31) also keeps the DC door machinery from
// prematurely force-opening the lock-free Conservatory Door (which would bypass
// Ironbark) or auto-pausing on the door-blocked timeout before the ~25s open. On
// completion boss-nav routes the tank through the now-open door to Alzzin.
//
// Persistent so a stray combat tick-gap can't rewind to step 0 and re-gossip —
// Ironbark removes his gossip npcflag on the first select, so a re-talk would
// find an empty menu and stall. SkipIfTargetMissing lets the gossip step skip
// once he has walked off / despawned; Optional degrades a non-firing script to
// the normal door-blocked stall (the door still opens, driven by his own AI).

void RegisterDireMaulEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(429, 1, "Ironbark opens the Conservatory Door")
                      .Anchored(/*orderIndex, doc-only*/ 4)
                      .Gossip(/*Ironbark the Redeemed*/ 14241, /*option*/ 0,
                              /*searchRadius*/ 20.0f)
                          .SkipIfTargetMissing()
                      // Ironbark's SmartAI opens the door with SetGoState(2) =
                      // GO_STATE_ACTIVE_ALTERNATIVE (not the usual ACTIVE=0), so
                      // wait for 2. Wide search: the tank holds at Ironbark's
                      // spawn while the door is ~187 yds east down the corridor,
                      // far beyond the 20yd GO-search default — without this the
                      // step never finds the GO and holds until timeout.
                      .WaitForGOState(/*Conservatory Door*/ 176907,
                                      /*GO_STATE_ACTIVE_ALTERNATIVE*/ 2,
                                      /*timeout*/ 90000, /*searchRadius*/ 250.0f)
                      .Persistent()
                      .Optional()
                      .Build());

    // --- Dire Maul North (map 429) — Gordok Courtyard & Inner Doors --------
    // The North wing is gated by two doors the party can't path through while
    // shut: the Gordok Courtyard Door (GO 177219) at the front of the courtyard
    // and the Gordok Inner Door (GO 177217) before King Gordok's chamber. Both
    // are GAMEOBJECT_TYPE_DOOR with SmartGameObjectAI: a gossip-hello SET_INST_DATA
    // (TYPE_NORTH_WING_PROGRESS = 1 courtyard / 2 inner) and the door's own
    // On-Update GO_SET_GO_STATE(0) swings it to GO_STATE_ACTIVE; autoclose is off
    // so it stays open. GameObject::Use() fires the AI's GossipHello BEFORE any
    // lock/key check (GameObject.cpp), so a plain UseGO opens them — no key
    // needed, even though both carry GO_FLAG_LOCKED (which is precisely why the
    // stock DC door machinery won't auto-force them; this event is the only path).
    //
    // CONDITIONAL, not Anchored — these doors sit directly ON the corridor (the
    // tank walks straight into them while pursuing the next boss). An anchored
    // objective-arrive (relevance 30) CANNOT fire here: the shut door truncates
    // the route short of any anchor placed at it, so the bot never "arrives", and
    // the door-blocked stall (the blocking-door value flags a shut door up to
    // ~80yd ahead) parks the tank with "a closed door is blocking the path". The
    // door-blocked TRIGGER stands down only when a CONDITIONAL event is due
    // (relevance 31 > 22), so an on-path door MUST be conditional to preempt it.
    //
    // No nav steps are needed: the door-blocked action itself MoveTo-delivers the
    // tank up to the door before parking (see the door walk-in fix), so by the
    // time the condition fires the bot is already at the door. The condition scan
    // is kept TIGHT (~= the UseGO search radius) so door-blocked does that
    // delivery first; only once the bot is within reach does the condition go
    // true, door-blocked stands down, and UseGO closes the last few yards (HopTo)
    // and clicks. A wide scan would strand the bot — UseGO has no long-range nav.
    // Open state is GO_STATE_ACTIVE (0) here (unlike the East door, which Ironbark
    // opens to 2). Optional so a misfire degrades to the normal door-blocked stall.

    out.push_back(EventBuilder(429, 2, "Open the Gordok Courtyard Door")
                      .Conditional(/*conditionId*/ 12)
                      .UseGO(/*Gordok Courtyard Door*/ 177219, /*searchRadius*/ 25.0f)
                      .WaitForGOState(177219, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 30000, /*searchRadius*/ 25.0f)
                      .Optional()
                      .Build());

    out.push_back(EventBuilder(429, 3, "Open the Gordok Inner Door")
                      .Conditional(/*conditionId*/ 13)
                      .UseGO(/*Gordok Inner Door*/ 177217, /*searchRadius*/ 25.0f)
                      .WaitForGOState(177217, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 30000, /*searchRadius*/ 25.0f)
                      .Optional()
                      .Build());
}

namespace
{
    constexpr uint32 DM_COURTYARD_DOOR = 177219;
    constexpr uint32 DM_INNER_DOOR = 177217;
    // Tight scan: see the CONDITIONAL note above. The condition is true only once
    // the bot is within reach of the (still-shut) door, so the door-blocked action
    // delivers the bot to the door first; then this preempts the stall and UseGO
    // (search 25yd) opens it. Keep this <= the event's UseGO search radius.
    constexpr float DM_DOOR_SCAN = 25.0f;

    // Generic on-path door condition: due iff the door GO is found within reach
    // and is still closed (GO_STATE_READY). Once UseGO opens it (-> ACTIVE) this
    // reads false and the event latches done. FindNearestGameObject inherently
    // localises to map 429 near the door, so no extra wing/coord gate is needed.
    bool GordokDoorShut(Player* bot, uint32 doorEntry, char const* which)
    {
        GameObject* door = bot->FindNearestGameObject(doorEntry, DM_DOOR_SCAN);

        // Throttled diagnostic (one line / 5s) so a live run shows whether the
        // door is in reach and its state. Lands in DungeonClear.log.
        static uint32 lastLog = 0;
        uint32 const now = getMSTime();
        if (getMSTimeDiff(lastLog, now) >= 5000)
        {
            lastLog = now;
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] Gordok door cond ({}): door={} state={}",
                      bot->GetName(), which, door ? "in-reach" : "far/MISSING",
                      door ? static_cast<int>(door->GetGoState()) : -1);
        }

        return door && door->GetGoState() == GO_STATE_READY;
    }

    bool GordokCourtyardDoor(Player* bot, AiObjectContext* /*context*/)
    {
        return GordokDoorShut(bot, DM_COURTYARD_DOOR, "courtyard");
    }

    bool GordokInnerDoor(Player* bot, AiObjectContext* /*context*/)
    {
        return GordokDoorShut(bot, DM_INNER_DOOR, "inner");
    }
}

void RegisterDireMaulConditions(EventConditionMap& out)
{
    out[12] = &GordokCourtyardDoor;
    out[13] = &GordokInnerDoor;
}
