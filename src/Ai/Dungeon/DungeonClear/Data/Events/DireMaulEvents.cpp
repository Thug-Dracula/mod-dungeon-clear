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

#include <atomic>

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

    // PanelAfterBoss anchors each door to a North-wing boss: it sorts the panel
    // row sensibly (the courtyard door after the three southern guards, the inner
    // door after Slip'kik / before the King's chamber) AND — crucially on this
    // wing-split map — scopes the row to the North wing, so these events don't
    // surface in the Dire Maul East/West panels (the boss list is wing-filtered;
    // an event whose anchor boss isn't in it is hidden). See DungeonClearChatActions.
    out.push_back(EventBuilder(429, 2, "Open the Gordok Courtyard Door")
                      .Conditional(/*conditionId*/ 12)
                      .UseGO(/*Gordok Courtyard Door*/ 177219, /*searchRadius*/ 25.0f)
                      .WaitForGOState(177219, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 30000, /*searchRadius*/ 25.0f)
                      .PanelAfterBoss(/*Guard Fengus*/ 14321)
                      .Optional()
                      .Build());

    out.push_back(EventBuilder(429, 3, "Open the Gordok Inner Door")
                      .Conditional(/*conditionId*/ 13)
                      .UseGO(/*Gordok Inner Door*/ 177217, /*searchRadius*/ 25.0f)
                      .WaitForGOState(177217, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 30000, /*searchRadius*/ 25.0f)
                      .PanelAfterBoss(/*Guard Slip'kik*/ 14323)
                      .Optional()
                      .Build());

    // --- Dire Maul West (map 429) — Immol'thar's five Crystal Generators ---
    // Immol'thar (11496) is held NON_ATTACKABLE behind a force field (GO 179503)
    // powered by five Crystal Generator pylons (BUTTON GOs). Each generator's
    // SmartGameObjectAI, once active, sets a bit in instance data TYPE_PYLONS_STATE
    // (field 2); at 0x1F (all five) the instance drops the field and clears
    // Immol'thar's (and the Highborne Summoners') NON_ATTACKABLE flag. The pylons
    // are scattered across the whole West wing — far beyond UseGO's HopTo range —
    // so each is a travel OBJECTIVE (BossRosterRegistry map-429 West patch,
    // OBJ(2..6)) that boss-nav delivers the tank to; this anchored event then
    // clicks it.
    //
    // ANCHORED (the objective-arrive drives it), not Conditional: there is no
    // door to preempt, and boss-nav must deliver the tank across the wing to each
    // pylon's anchor first. Persistent so a combat tick-gap at a guarded pylon
    // can't rewind the event to step 0 and re-click (the instance ORs the bit, so
    // a re-fire is idempotent, but re-clicking a spent BUTTON is best avoided).
    // The Wait(6000) covers the generator's ~5s SmartAI activation delay so the
    // bit is set before the tank moves on (and, on the last pylon, before it
    // reaches Immol'thar). Optional: a misfire degrades to the tank standing at
    // the still-shielded boss until the pylon is dealt with.
    //
    // Each event clears the crystal's guards/treants THEN clicks it — the proven
    // Uldaman keeper-altar pattern (ClearRadius -> MoveTo -> UseGO), NOT a bare
    // UseGO. The trick that makes "clear AND click" not deadlock is matching the
    // objective's arriveRadius to the ClearRadius (BossRosterRegistry sets the
    // crystal objectives' arriveRadius to 45, > this 40yd ClearRadius): while the
    // ClearRadius drives the tank around the clear zone chasing guards, the tank
    // stays within arriveRadius so the at-objective action keeps OWNING the tick
    // (relevance 30) and runs the event's steps in sequence. A small arriveRadius
    // (the old 10 vs a 50-70 ClearRadius) let the tank slip out of "arrived"
    // mid-clear, so engage-trash/Advance competed for the tick — the live
    // back-and-forth deadlock. Steps:
    //   0) ClearRadius(40): kill the ~7-8 elemental guards (Arcane Aberration
    //      11480 / Mana Remnant 11483; the Remnants Blink ~20yd, so 40 keeps a
    //      blinker in the kill zone) plus any treants within 40yd (at generator 1,
    //      the Warpwood entrance pack). Counts only REACHABLE hostiles, so a mob
    //      blinked somewhere unreachable can't hang it. 120s timeout backstop.
    //   1) MoveTo the crystal (radius 6): close the last yards via a fresh HopTo,
    //      so a long-range route that truncated short of the dais still reaches.
    //   2) UseGO: click the generator (sets the pylon bit ~5s later).
    //   3) Wait: cover that ~5s SmartAI activation delay before advancing.
    // Persistent so a combat tick-gap during the guard fight can't rewind to step
    // 0 and re-click. Optional so a stall degrades to moving on.
    //
    // NOTE (verify live): the generator trigger is SMART_EVENT_UPDATE (a timer),
    // not an explicit on-use event. If the generators auto-activate on spawn
    // rather than on click, the UseGO is a harmless no-op (the bit is idempotent).
    struct PylonObjective { uint32 eventId; uint32 goEntry; float x, y, z; char const* name; };
    static constexpr PylonObjective kPylons[] = {
        { 4, 177259,   12.94f, 277.93f,  -8.93f, "Destroy Demon Crystal (Generator 1)" },
        { 5, 177257,  -92.35f, 442.67f,  28.55f, "Destroy Demon Crystal (Generator 2)" },
        { 6, 177258,  121.22f, 429.09f,  28.45f, "Destroy Demon Crystal (Generator 3)" },
        { 7, 179504,   78.14f, 737.40f, -24.62f, "Destroy Demon Crystal (Generator 4)" },
        { 8, 179505, -155.43f, 734.17f, -24.62f, "Destroy Demon Crystal (Generator 5)" },
    };
    for (PylonObjective const& pyl : kPylons)
    {
        out.push_back(EventBuilder(429, pyl.eventId, pyl.name)
                          .Anchored(/*orderIndex, doc-only*/ pyl.eventId)
                          .ClearRadius(pyl.x, pyl.y, pyl.z, /*radius*/ 40.0f,
                                       /*zBand*/ 15.0f)
                              .Timeout(120000)
                          .MoveTo(pyl.x, pyl.y, pyl.z, /*radius*/ 6.0f)
                          .UseGO(pyl.goEntry, /*searchRadius*/ 12.0f)
                          .Wait(/*pylon activation delay*/ 6000)
                          .Persistent()
                          .Optional()
                          .Build());
    }

    // --- Dire Maul West (map 429) — Warpwood entrance SWEEP ----------------
    // The entrance hall is HUGE — the Petrified Treant / Petrified Guardian (plus
    // Ironbark Protector) packs blanket the whole room: x -97..+135 (≈232yd wide)
    // by y 185..360 (≈175yd deep), in three latitude bands. ONE ClearRadius (or
    // even the old two, both parked at the entrance lip y~200) reaches only the
    // few packs by the door — the model shows ~16 treant packs, of which the old
    // sweep's two y=200/r45 circles covered barely two, so the clear no-op'd in
    // 0ms and left the room full. The Eldreth ghosts (behind Tendris, y>=410) and
    // the imprisoned Highborne (z+28) must NOT be woken, so a single huge radius
    // is out. Instead SWEEP a GRID of small clears that together tile the treant
    // field while each stays clear of the Eldreth/Highborne (BossRosterRegistry
    // OBJ(8)..OBJ(14)):
    //   Band 1 entrance  (y~195, z~-4): west / centre / east  — covers the door
    //                    packs t,k,l,m,u,y + the flanking Ironbark Protectors.
    //   Band 2 mid hall  (y~270, z~-8): west / east — covers c,d,e,g,j (west) and
    //                    f,h,i (east); radii kept just short of generator 1's own
    //                    clear at (13,278) so the two don't fight over the dais.
    //   Band 3 approach  (y~357, z~-4): west / east — covers the far z,o packs and
    //                    their Ironbarks; r kept tight so the y>=410 Eldreth stay
    //                    asleep.
    // Each stop's ClearRadius engages the local pack (treants are aggro-on-sight
    // and cluster on the tank) and the gate holds until that stop is clear before
    // the run advances. zBand 20 (centred on the band's floor) excludes the z+28
    // Highborne. All three bands order BEFORE generator 1 (BossRosterRegistry
    // orderOverride 2/3/4 < Gen1's 5), so the tank tiles the room front-to-back in
    // one contiguous northward pass while the y>=410 Eldreth / Tendris sleep.
    //
    // Key: every waypoint sits on OPEN FLOOR where treants stand (not the off-mesh
    // crystal dais), so the tank reaches it closely — no parks-short deadlock —
    // and "arrives" AMONG the treants, so the ClearRadius engages instead of
    // completing in 0ms. arriveRadius (30) < ClearRadius (per-stop) is fine
    // (Uldaman keeper numbers); the small radius keeps the tank fighting in place
    // as the pack closes, so it does not chase far and thrash. ClearRadius-only;
    // REACHABLE-only; Persistent; Optional; generous timeout.
    struct SweepWaypoint { uint32 eventId; float x, y, z, radius; char const* name; };
    static constexpr SweepWaypoint kEntranceSweep[] = {
        // Band 1 — entrance lip (west / centre / east).
        {  12, -97.0f, 202.0f, -4.0f, 45.0f, "Clear the Warpwood entrance (west)" },
        {  11, -15.0f, 192.0f, -3.5f, 45.0f, "Clear the Warpwood entrance (centre)" },
        {  13, 128.0f, 200.0f, -4.0f, 45.0f, "Clear the Warpwood entrance (east)" },
        // Band 2 — mid hall (west / east); radii short of generator 1's clear.
        {  14, -44.0f, 280.0f, -7.5f, 48.0f, "Clear the Warpwood hall (west)" },
        {  15,  60.0f, 285.0f, -8.0f, 45.0f, "Clear the Warpwood hall (east)" },
        // Band 3 — northern approach (west / east); tight so the Eldreth sleep.
        {  16, -93.0f, 357.0f, -4.0f, 42.0f, "Clear the Warpwood approach (west)" },
        {  17, 126.0f, 357.0f, -4.0f, 42.0f, "Clear the Warpwood approach (east)" },
    };
    for (SweepWaypoint const& wp : kEntranceSweep)
    {
        out.push_back(EventBuilder(429, wp.eventId, wp.name)
                          .Anchored(/*orderIndex, doc-only*/ wp.eventId)
                          .ClearRadius(wp.x, wp.y, wp.z, wp.radius,
                                       /*zBand*/ 20.0f)
                              .Timeout(180000)
                          .Persistent()
                          .Optional()
                          .Build());
    }

    // --- Dire Maul West (map 429) — Crescent Key doors ---------------------
    // Two locked doors (GO 177221 after Tendris, GO 179550 before Immol'thar)
    // gate the West path. Both are GAMEOBJECT_TYPE_DOOR with lock 1562 (the
    // Crescent Key, item 18249) — but server-side GameObject::Use() on a DOOR
    // toggles it open with NO lock/key check (the key is a client-only gate), so
    // a bot UseGO opens them without a key, exactly like the Gordok doors. The
    // stock DC door machinery refuses them (DcDoorPolicy suppresses bare-hands on
    // GO_FLAG_LOCKED), so this CONDITIONAL event is the only path — and being
    // on-path it must be conditional to preempt the door-blocked stall (rel 31 >
    // 22; an anchored objective can't fire because the shut door truncates the
    // route short of it). Same shape as the Gordok doors. (The third lock-1562
    // door, 179549 at the entrance, is off the West boss path and is ignored.)
    out.push_back(EventBuilder(429, 9, "Open the Crescent Key Door (lower)")
                      .Conditional(/*conditionId*/ 14)
                      .UseGO(/*after Tendris*/ 177221, /*searchRadius*/ 25.0f)
                      .WaitForGOState(177221, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 30000, /*searchRadius*/ 25.0f)
                      .PanelAfterBoss(/*Tendris Warpwood*/ 11489)
                      .Optional()
                      .Build());

    out.push_back(EventBuilder(429, 10, "Open the Crescent Key Door (upper)")
                      .Conditional(/*conditionId*/ 15)
                      .UseGO(/*before Immol'thar*/ 179550, /*searchRadius*/ 25.0f)
                      .WaitForGOState(179550, /*GO_STATE_ACTIVE*/ 0,
                                      /*timeout*/ 30000, /*searchRadius*/ 25.0f)
                      .PanelAfterBoss(/*Magister Kalendris*/ 11487)
                      .Optional()
                      .Build());
}

namespace
{
    constexpr uint32 DM_COURTYARD_DOOR = 177219;
    constexpr uint32 DM_INNER_DOOR = 177217;
    constexpr uint32 DM_CRESCENT_DOOR_LO = 177221;  // after Tendris
    constexpr uint32 DM_CRESCENT_DOOR_HI = 179550;  // before Immol'thar
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
        // door is in reach and its state. Lands in DungeonClear.log. atomic because
        // bot AI ticks run on the MapUpdate.Threads pool — the throttle stamp is
        // read/written from multiple map threads (the check-then-set race is benign).
        static std::atomic<uint32> lastLog{0};
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

    // Crescent Key doors (West) reuse the identical on-path door predicate —
    // due iff the lock-1562 door is in reach and still shut. GameObject::Use on
    // a DOOR ignores the lock server-side, so the event's UseGO opens it.
    bool CrescentDoorLower(Player* bot, AiObjectContext* /*context*/)
    {
        return GordokDoorShut(bot, DM_CRESCENT_DOOR_LO, "crescent-lo");
    }

    bool CrescentDoorUpper(Player* bot, AiObjectContext* /*context*/)
    {
        return GordokDoorShut(bot, DM_CRESCENT_DOOR_HI, "crescent-hi");
    }
}

void RegisterDireMaulConditions(EventConditionMap& out)
{
    out[12] = &GordokCourtyardDoor;
    out[13] = &GordokInnerDoor;
    out[14] = &CrescentDoorLower;
    out[15] = &CrescentDoorUpper;
}
