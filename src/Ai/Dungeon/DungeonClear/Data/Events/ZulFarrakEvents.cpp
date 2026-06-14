/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// --- ZulFarrak (map 209) — the TEMPLE (pyramid) event --------------------
// The scripted set-piece between Witch Doctor Zum'rah and the final boss Chief
// Ukorz Sandscalp. Verified from the core scripts (instance_zulfarrak.cpp /
// zulfarrak.cpp): the door to Ukorz stays sealed until the party plays this out,
// so without it the tank bee-lines to Ukorz, hits the closed door and stalls.
//
// The event is ONE persistent anchored step list (eventId 1) tied to the temple
// objective anchor (BossRosterRegistry, at the Sandfury Executioner's spot atop
// the great staircase, ordered at Ukorz's bit 7 so the tank goes up FIRST). It is
// Persistent + the at-objective trigger is sticky, so the SAME progress survives
// the many wave/boss fights (each a >1s gap on the combat engine that would
// otherwise rewind the chain) and the tank may roam far from the anchor — down
// the stairs to the bosses and back to the NPCs — while the event drives.
//
// The scripted phases (instance DATA_PYRAMID), all driven by the steps below:
//   1. Kill the Sandfury Executioner (7274) guarding the summit, then open a
//      Troll Cage (GO 141070-141074). Using one cage (go->Use cheats the lock —
//      no key needed) fires go_troll_cageAI::GossipHello, which frees Bly's band
//      (passive in cages) and starts the assault.
//   2. The cages / executioner sit BACK from the staircase, but the waves charge
//      UP the ramp and hit the freed NPC crew (who stage at the ramp head) first —
//      if the party loiters at the summit it never aggros the waves and the NPCs
//      die. So we move forward onto the ramp head (the crew's staging line,
//      verified from initBlyCrewMember) and hold there to tank the waves off the
//      NPCs.
//   3. Wave 3 spawns at the BOTTOM of the stairs together with the temple bosses
//      Nekrum Gutchewer (7796) and Shadowpriest Sezz'ziz (7275); the NPC crew
//      moves down to engage. We descend (engage steps drive the long walk-in) and
//      help kill them.
//   4. With the trolls dead the crew settles at the bottom. We talk to the goblin
//      Weegli Blastfuse (7607) FIRST — his gossip sends him to blow open the door
//      to Ukorz — then dwell ~10s before provoking Bly. (Per live experience: a
//      ~10s gap is enough; Bly's faction-flip of the crew only fires ~10s after
//      HIS gossip, by which point Weegli is well clear, so the door still opens. A
//      full wait-for-Weegli-despawn is unnecessary.)
//   5. Then talk to Sergeant Bly (7604, human) — his band turns hostile; killing
//      Bly ends the event (DATA_PYRAMID = DONE, door to Ukorz open). The clear
//      then latches the objective and proceeds to Chief Ukorz naturally.
//
// Gossip uses the executor's opcode-forgery path (proven on Shadowfang Keep), and
// the engage steps reuse the normal engage pipeline (long-range path + combat +
// follower assist) so the party fights alongside the NPC helpers.

namespace
{
    constexpr uint32 ZF_EXECUTIONER = 7274;
    // Any one cage starts the event; 141073 sits ~9yd off the objective anchor.
    constexpr uint32 ZF_TROLL_CAGE = 141073;
    constexpr uint32 ZF_NEKRUM = 7796;   // temple boss (wave 3, no static spawn)
    constexpr uint32 ZF_SEZZIZ = 7275;   // temple boss (wave 3, no static spawn)
    constexpr uint32 ZF_WEEGLI = 7607;   // goblin — blows the door to Ukorz
    constexpr uint32 ZF_BLY = 7604;      // human — final fight ends the event

    // Where to hold during the waves: PARTWAY DOWN the ramp, clearly IN FRONT of
    // Bly's crew (they stage at the head, ~y1263 z41.5), facing the waves coming
    // up the stairs. This is essential — the party does NOT auto-assist the NPCs,
    // so unless a party member takes the first aggro nobody engages and the crew
    // dies. Sitting down-ramp of the crew puts the tank in the waves' path so it
    // grabs them first. Coords interpolate the staircase (head y1263/z41.5 ->
    // bottom y1228/z~10, verified from initBlyCrewMember + the PRE_WAVE_3 NPC
    // moves); a generous arrive radius tolerates the per-step z of the stairs.
    constexpr float ZF_RAMP_X = 1886.0f;
    constexpr float ZF_RAMP_Y = 1250.0f;
    constexpr float ZF_RAMP_Z = 30.0f;

    // The wave sequence (cages open -> wave 3 / bosses spawn) runs several
    // minutes; a generous timeout keeps the long survive-the-waves wait from
    // escalating to a stall. The NPC gossips / door-blow likewise take a while.
    constexpr uint32 ZF_WAVES_TIMEOUT = 900000;  // 15 min
    constexpr uint32 ZF_NPC_TIMEOUT = 180000;    // 3 min
    // Lead time between gossiping Weegli (door) and Bly (fight) — enough that
    // Bly's faction-flip of the crew can't catch Weegli mid-walk (live-verified).
    constexpr uint32 ZF_WEEGLI_LEAD_MS = 10000;  // 10 s
}

void RegisterZulFarrakEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(209, 1, "Temple (Executioner / Bly's Band event)")
                      .Anchored(7)
                      .Persistent()
                      // 1. Clear the summit guard, then crack a cage to begin.
                      .KillCreatureEngage(ZF_EXECUTIONER)
                      .UseGO(ZF_TROLL_CAGE, /*searchRadius*/ 25.0f)
                      // 2. Move down the ramp, IN FRONT of the NPC crew, so the
                      //    tank takes the waves' first aggro (the party doesn't
                      //    auto-assist the NPCs), then hold there until Sezz'ziz
                      //    spawning signals wave 3 has begun at the bottom.
                      .MoveTo(ZF_RAMP_X, ZF_RAMP_Y, ZF_RAMP_Z, /*radius*/ 10.0f)
                      .WaitForSpawn(ZF_SEZZIZ, /*wantAlive*/ true).Timeout(ZF_WAVES_TIMEOUT)
                      // 3. Descend and help the crew kill the temple bosses.
                      .KillCreatureEngage(ZF_NEKRUM, /*count*/ 1, /*searchRadius*/ 250.0f)
                      .KillCreatureEngage(ZF_SEZZIZ, /*count*/ 1, /*searchRadius*/ 250.0f)
                      // 4. Goblin FIRST — opens the door — then a short dwell so
                      //    Bly's later faction-flip can't catch Weegli mid-walk.
                      //    WaitTargetStill: the crew's gossip is offered while they
                      //    are still walking DOWN to the temple floor — wait until
                      //    Weegli settles at his final spot before talking, or he is
                      //    interrupted mid-descent. SkipIfTargetMissing: if Weegli
                      //    died the boss is unreachable, but don't deadlock — skip
                      //    and still take Bly (the human can re-open / `dc skip`).
                      .Gossip(ZF_WEEGLI, /*option*/ 0, /*searchRadius*/ 40.0f)
                          .Timeout(ZF_NPC_TIMEOUT).WaitTargetStill().SkipIfTargetMissing()
                      .Wait(ZF_WEEGLI_LEAD_MS)
                      // 5. Human — starts the fight; killing Bly ends the event.
                      //    WaitTargetStill so we don't provoke Bly mid-descent;
                      //    SkipIfTargetMissing on the gossip + the engage gate's
                      //    own dead-check mean a dead Bly just completes the event
                      //    (all helpers gone => the clear continues to Ukorz).
                      .Gossip(ZF_BLY, /*option*/ 0, /*searchRadius*/ 40.0f)
                          .Timeout(ZF_NPC_TIMEOUT).WaitTargetStill().SkipIfTargetMissing()
                      .KillCreatureEngage(ZF_BLY, /*count*/ 1, /*searchRadius*/ 80.0f)
                      .Build());
}
