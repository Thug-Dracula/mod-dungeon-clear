/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearActions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Position.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproach.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproachIo.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDoorPolicy.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPathWorker.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/StridedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/SwimPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearStateValues.h"
#include "Playerbots.h"
#include "DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

using namespace DcActionShared;

namespace
{
    // Trail-arrival tolerance. A follower gliding up the tank's breadcrumb trail
    // stops HOLDING once it is within this many yards of its target crumb, so its
    // true rest distance from the tank is the lag PLUS up to this slack. The
    // scout-lag spread clamp below must budget for it (see kScoutLagSpreadMargin)
    // or the followers park just outside the tank's readiness gate and deadlock.
    constexpr float kTrailArrival = 4.0f;

    // True while a continuous escort-spline glide is in flight. The follower trail
    // re-issue guards key off this (NOT the LastMovement wait) so a healthy glide
    // is left to finish and chain seamlessly into the next window, exactly as
    // DcAdvanceAction does for the tank — relaunching MoveSplinePath every tick is
    // what made followers half-step.
    bool TrailSplineRunning(Player* bot)
    {
        MotionMaster* mm = bot->GetMotionMaster();
        return mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE &&
               bot->isMoving();
    }

    // Issue the follower's centered breadcrumb window (live pos .. the crumb `lag`
    // yards behind the tank) as ONE continuous escort spline — the same smooth
    // glide the tank's advance uses, replacing the per-tick single-point MoveTo
    // that made followers visibly half-step at each crumb. Returns true iff a
    // spline was launched (a usable >= 2-point window existed and SplinePath took
    // it); callers fall back to the single-point step / Follow fan on false.
    bool IssueTrailGlide(PlayerbotAI* botAI, Player* bot, float lag)
    {
        std::vector<Position> trail;
        if (!DcLeaderSignal::GetLeaderScoutTrail(bot, lag, trail) || trail.size() < 2)
            return false;
        Movement::PointsArray window;
        window.reserve(trail.size());
        for (Position const& p : trail)
            window.emplace_back(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ());
        return DcMovement::SplinePath(botAI, window);
    }
}

bool DungeonClearFollowTankAction::Execute(Event /*event*/)
{
    ObjectGuid& followedTank =
        context->GetValue<ObjectGuid>(DcKey::FollowedTank)->RefGet();

    Player* tank = AI_VALUE(Player*, DcKey::PartyTank);
    if (!tank || tank == bot)
    {
        // No DC tank: tear down the leftover continuous MoveFollow we
        // installed while following. MoveFollow is a persistent MotionMaster
        // order; once the tank's DC flag clears, this action stops being
        // selected and nothing else cancels it for a self-bot (its ordinary
        // follow targets itself and no-ops without clearing), so it would
        // stay glued to the tank. Clear it once, forget the tank, and let the
        // bot revert to stock behavior (a self-bot then stands still as the
        // leader; normal bots fall back to following their master).
        if (!followedTank.IsEmpty())
        {
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] follow-tank: released (DC tank gone) -> cleared "
                     "follow generator (selfRealPlayer={})",
                     bot->GetName(), botAI && botAI->IsRealPlayer() ? 1 : 0);
            followedTank = ObjectGuid::Empty;
            // Cleanly torn down by us -> drop the orphan-reaper mark; there is no
            // longer a follow generator for it to chase down.
            DcFollowerLifecycle::UnmarkFollowing(bot->GetGUID());
        }
        return false;
    }

    // Leader is dropping down a narrow one-way hole the followers can't path
    // (Wailing Caverns' return-fall off Verdan's shelf). HOLD here at the ledge
    // top instead of following: a MoveFollow toward the now-far-below tank finds
    // no navmesh route and produces a degenerate path that clips the follower
    // straight down through the hole wall. StopBot(Hold) tears down any follow
    // generator already installed (otherwise it keeps driving the clip). The
    // leader teleports the whole party to the landing the instant it lands (the
    // DropInHole RunStep gate), so the right behavior until then is to stand
    // still. Keep the teardown/orphan bookkeeping live so a generator we installed
    // is still cancelled when the DC tank later goes away.
    if (DcLeaderSignal::IsLeaderDroppingInHole(bot))
    {
        DcMovement::StopBot(bot, DcMovement::Stop::Hold);
        followedTank = tank->GetGUID();
        DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] follow-tank: leader dropping down the hole -> holding at "
                  "the ledge top until it lands", bot->GetName());
        return true;
    }

    // Loot yield (with commit-timeout). Followers run ONLY this action while DC
    // is active (their own DC is never enabled — `dc on` is tank-only — so the
    // advance/engage triggers are all inactive for them; follow-tank, relevance
    // 25, outranks the loot actions (open loot 8, move to loot 7, loot 6) every
    // non-combat tick). Without yielding here, the instant `move to loot` walks
    // a follower past followDistance toward a corpse, follow-tank wins the next
    // tick and yanks it straight back — so followers can only ever pick up
    // corpses already sitting inside the tank's follow bubble, and a corpse a
    // few yards out never gets looted at all. The tank pauses between pulls to
    // let loot resolve (see the advance loot-yield in Execute); mirror that here
    // so the party can actually path out to corpses during that window.
    //
    // Stepping aside on EITHER loot flag covers the whole pickup lifecycle:
    // "has available loot" is true only at ~3-15yd and flips FALSE at ~3yd when
    // "can loot" flips TRUE, so yielding on just one would re-assert follow at
    // the 3yd boundary and pull the bot off the corpse before `open loot` ran
    // — the same oscillation the tank's advance yield avoids. The stack is
    // populated by `add all loot` (relevance 5), which gets its tick whenever
    // the follower is already within followDistance and Follow() no-ops (returns
    // false) — i.e. while clustered on the resting tank, exactly when corpses
    // are nearby. The timeout stops a follower parking forever on a corpse it
    // can't finish (group-roll pending, bags full) and being left behind, which
    // would also stall the tank on its party-spread gate.
    //
    // Strip already-given-up loot first (above the loot pipeline's relevance),
    // so a corpse this follower abandoned can't keep the flags below true and
    // re-arm the yield each time it drifts back within lootDistance of the
    // tank — the corpse<->tank ping-pong.
    DcLootPolicy::StripSkippedLoot(botAI);
    // Proactively skip a corpse with nothing takeable for this follower (un-
    // finishable group-roll/reserved loot, or below DungeonClear.LootMinQuality)
    // BEFORE it walks over — so it never steps off follow for it and never adds
    // to the tank's IsAnyPartyMemberLooting wait. Event-driven counterpart to
    // the camp/timeout cutoffs, which only fire after the walk is wasted.
    DcLootPolicy::MaybeSkipUnworthyLoot(botAI);
    // Fast-skip a corpse this follower has been camped on too long instead of
    // waiting out the full yield timeout: an un-finishable corpse (group-roll
    // items pending, bags full) otherwise wastes 15s here AND keeps the tank's
    // IsAnyPartyMemberLooting true, stalling the whole party on it.
    DcLootPolicy::MaybeGiveUpCampedLoot(botAI, DC_LOOT_CAMP_TIMEOUT_MS, DC_LOOT_GIVEUP_TTL_MS);
    uint32& lootYieldStart =
        context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get().lootYieldStartMs;
    bool const lootYield =
        AI_VALUE(bool, DcKey::Stock::HasAvailableLoot) || AI_VALUE(bool, DcKey::Stock::CanLoot);
    if (lootYield)
    {
        uint32 const now = getMSTime();
        if (lootYieldStart == 0)
            lootYieldStart = now;

        if (now - lootYieldStart < DC_LOOT_YIELD_TIMEOUT_MS)
        {
            // Hand the tick to move-to-loot / open-loot. Don't issue a follow
            // move that would drag the bot off the corpse.
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] follow-tank yielding: loot in progress ({}ms)",
                      bot->GetName(), now - lootYieldStart);
            return false;
        }
        // Waited long enough — give up on THIS corpse (blacklist it so the
        // flags drop next tick and we stop being yanked back to it), then resume
        // following to rejoin the tank. Leave lootYieldStart expired so we keep
        // following until the flags clear (which resets the timer).
        DcLootPolicy::GiveUpCurrentLoot(botAI, DC_LOOT_GIVEUP_TTL_MS);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] follow-tank loot-yield timed out after {}ms -> giving up on corpse, following",
                 bot->GetName(), now - lootYieldStart);
    }
    else
    {
        lootYieldStart = 0;  // not looting -> reset the commit timer
    }

    // In DYNAMIC pull mode, trail the tank at a lag distance while it scouts toward
    // the next pack and decides Leeroy vs Advanced (leader out of combat, phase
    // Idle). The tight follow bubble otherwise dragged the party onto the tank's
    // heels and into the pack's aggro arc before the tank committed, accidentally
    // pulling — the reported bug. This can NOT be done through Follow()'s distance
    // arg: Follow() early-outs whenever the bot is already inside the global
    // followDistance and refuses to re-issue an existing follow on the same target,
    // so it can only ever CLOSE a gap, never widen one (the previous attempt was a
    // silent no-op). Manage the spacing directly instead: hold inside the lag
    // bubble (let the tank pull ahead), and when the tank has pulled beyond it step
    // up to the lag ring and stop rather than charging back into the tight bubble.
    // The instant the tank commits — enters combat (Leeroy) or marks a camp
    // (Advanced hands the party to hold-at-camp, so follow-tank stands down) — this
    // gate flips false and the party reverts to the tight follow / camp hold below.
    if (DcLeaderSignal::IsLeaderDynamicScouting(bot))
    {
        // The scout-lag deliberately PARKS followers `lag` yards behind the tank
        // (the `toTank <= lag` hold branch below stops them there). But the tank's
        // own between-pulls gate (DcAdvanceAction::TryBetweenPullsRest ->
        // IsBetweenPullsReady -> GetSpreadGate) refuses to advance until every
        // member is within DungeonClear.PartyMaxSpread. If the configured lag
        // exceeds that spread the two gates deadlock: the followers hold at the lag
        // ring OUTSIDE the spread the tank requires, so the tank waits forever for a
        // party that, by design, is never closing — the reported hang when
        // PartyMaxSpread is lowered toward PullDynamicPartyLag. Never lag farther than
        // the spread the tank will accept.
        //
        // The margin is NOT cosmetic: a follower's true rest distance is the lag
        // PLUS the trail-arrival slack (it HOLDS as soon as it is within
        // kTrailArrival of its crumb at `lag` behind the tank — see the arrival
        // hold below), so it can settle ~lag + kTrailArrival behind a stopped tank.
        // A bare `lag <= maxSpread` therefore still deadlocked at spread 15 / lag 15
        // (clamped 12, but resting ~16 > 15). Budget the arrival slack plus a couple
        // yards of walked-vs-straight / tick jitter so the held follower always
        // lands strictly inside the gate. At the default 25yd spread / 15yd lag this
        // is a no-op (15 < 25 - 6).
        constexpr float kScoutLagSpreadMargin = kTrailArrival + 2.0f;
        float const maxSpread = DcSettings::GetFloat(bot, "PartyMaxSpread");
        float const lag =
            std::min(DcSettings::GetFloat(bot, "PullDynamicPartyLag"),
                     std::max(2.0f, maxSpread - kScoutLagSpreadMargin));
        // 3D, NOT 2D: the tank's spread gate (IsPartyReady) measures GetDistance,
        // which is 3D — so the hold metric here MUST match it or they diverge on a
        // ramp/incline. A follower directly down-slope of the tank reads a small 2D
        // gap (it would hold) while its true 3D distance — what the gate enforces —
        // blows past PartyMaxSpread, and the tank waits forever for a follower that,
        // measured in 2D, thinks it is already close enough. This is the "worst on
        // inclines and ramps" deadlock. The trail-point accumulation (GetExactDist,
        // 3D) and the arrival hold below are already 3D; this was the last 2D read.
        float const toTank = bot->GetExactDist(tank);
        // Keep the teardown/orphan-reaper bookkeeping live across the whole window
        // so a follow generator we (or a prior tick) installed is still cancelled
        // when the DC tank goes away.
        followedTank = tank->GetGUID();
        DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
        if (toTank <= lag)
        {
            // Inside the lag bubble: hold position. Tear down any leftover
            // continuous MoveFollow so the bot actually stops instead of creeping
            // back to the tight followDistance.
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            DC_PULL_TRACE("[DC:{}] scout-lag: holding {:.1f}yd behind tank (lag {:.1f})",
                          bot->GetName(), toTank, lag);
            // Return FALSE, not true: the bot is already stopped, so yield the tick
            // to the lower-relevance pipeline (eat/drink, loot) exactly as stock
            // Follow() does on its "no need to follow" in-range early-out. Consuming
            // the tick here (return true) suppressed the out-of-combat rest routine
            // — the party held forever at the lag ring and never drank, deadlocking
            // the tank's between-pulls wait on the party's mana.
            return false;
        }
        // Tank pulled beyond the lag: step up to a point `lag` yards behind the
        // tank ALONG ITS BREADCRUMB TRAIL — the ground the tank actually walked,
        // which its escort spline already corridor-centered. The earlier version
        // projected a geometric lag point off the tank (tx + bearing*lag) and
        // MoveTo'd it through the raw PathGenerator: no corridor centering, so
        // followers cut their own wall-hugging corners, and the projected point
        // itself (bot's own Z, straight off the tank) could land on a ledge lip —
        // the reported "hugging walls / falling off ledges in dynamic pull". Trail
        // points are reachability-gated centered crumbs, so the move stays on the
        // safe route the tank already cleared.
        Position trailPoint;
        if (DcLeaderSignal::GetLeaderScoutTrailPoint(bot, lag, trailPoint))
        {
            // ARRIVAL HOLD. The hold gate above measures STRAIGHT-LINE distance to
            // the tank (toTank <= lag), but the trail point is the crumb at `lag`
            // yards of WALKED (path) distance behind the tank. On a curved corridor
            // the two disagree: a crumb 15yd back along the trail can sit ~16yd
            // straight-line from the tank, so a follower standing ON that crumb
            // still reads toTank > lag and never satisfies the hold gate. Without
            // this guard it re-issues MoveTo to the crumb it already occupies every
            // tick, micro-stepping around it forever — the reported "two steps
            // forward, two steps back" dance — and, because it's perpetually
            // "moving", never sits to drink/eat, stalling the tank's between-pulls
            // rest gate on its mana. So: if the bot has effectively reached the
            // trail point, HOLD here (same teardown + tick-yield as the in-bubble
            // branch) instead of demanding a straight-line gate it can't meet while
            // parked on a curved crumb. The slack is just the path-vs-straight
            // curvature over one lag; a few yards covers it without parking short.
            if (bot->GetExactDist(&trailPoint) <= kTrailArrival)
            {
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
                DC_PULL_TRACE("[DC:{}] scout-lag: holding at trail point "
                              "({:.1f}yd behind tank, lag {:.1f})",
                              bot->GetName(), toTank, lag);
                return false;
            }
            // Smooth glide along the centered crumb trail. Leave a healthy escort
            // glide in flight alone so it chains seamlessly instead of relaunching
            // (and stuttering) every tick, then ride the whole crumb window as ONE
            // continuous spline rather than the per-crumb single-point MoveTo below.
            if (TrailSplineRunning(bot))
                return true;
            if (IssueTrailGlide(botAI, bot, lag))
            {
                DC_PULL_DEBUG("[DC:{}] scout-lag: gliding tank's centered breadcrumbs "
                              "(was {:.1f}yd behind, lag {:.1f})",
                              bot->GetName(), toTank, lag);
                return true;
            }
            // normal_only: reject (don't straight-line to) a point that isn't
            // reachable over a real navmesh path. Crumbs are already gated for
            // reachability, but keep the guard as a belt-and-braces backstop.
            // Reached only when the glide window was too short / unreachable.
            if (DcMoveTo(bot->GetMapId(), trailPoint.GetPositionX(),
                       trailPoint.GetPositionY(), trailPoint.GetPositionZ(),
                       false, false, /*normal_only=*/true))
            {
                DC_PULL_DEBUG("[DC:{}] scout-lag: trailing tank along breadcrumbs "
                              "(was {:.1f}yd behind, lag {:.1f})",
                              bot->GetName(), toTank, lag);
                return true;
            }
        }
        // No trail yet (pull mode just toggled on, tank hasn't moved) or the trail
        // point was unreachable: fall through to a normal follow so the party never
        // gets permanently stranded.
    }

    // Room-aggro skirt for followers. While the leader clears a room before a boss
    // whose engage drags the WHOLE room into combat, the tank routes its OWN
    // approach AROUND the boss's aggro sphere (RoomAggroSkirtPoint). A follower
    // close-following the tank, though, makes a STRAIGHT line to it — and if the
    // tank is on the far side of the sphere (or just walked around it), that line
    // cuts through the aggro range and wakes the boss even though the tank dodged
    // it: the reported failure. So when the direct line to the tank clips the
    // sphere, walk the follower around it on the same short-arc detour the tank
    // uses (AggroSafeApproachPoint with the TANK as the move target), and revert to
    // the normal follow the instant a straight shot at the tank is clear. Skipped
    // entirely outside an active room clear (the lookup is a cheap no-op). The
    // dynamic scout-lag branch above already keeps the party on the tank's safe
    // breadcrumb trail in pull mode, so this only governs the tight close-follow.
    {
        Position bossCenter;
        float safeRadius = 0.0f;
        if (DcLeaderSignal::GetLeaderRoomAggroSphere(bot, bossCenter, safeRadius) &&
            DcEngageGeometry::NeedsRoomAggroSkirt(
                bot->GetPositionX(), bot->GetPositionY(),
                tank->GetPositionX(), tank->GetPositionY(),
                bossCenter.GetPositionX(), bossCenter.GetPositionY(), safeRadius))
        {
            if (std::optional<Position> wp = DcEngageGeometry::AggroSafeApproachPoint(
                    bot, bossCenter.GetPositionX(), bossCenter.GetPositionY(),
                    bossCenter.GetPositionZ(), safeRadius, tank))
            {
                // Keep the teardown/orphan-reaper bookkeeping live so a leftover
                // continuous MoveFollow is still cancelled when the DC tank goes
                // away; the point-move below supersedes the follow generator (same
                // as the scout-lag trail branch above — no explicit Stop needed).
                followedTank = tank->GetGUID();
                DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
                bool const moved = DcMoveTo(bot->GetMapId(), wp->GetPositionX(),
                                          wp->GetPositionY(), wp->GetPositionZ(),
                                          false, false, /*normal_only=*/false);
                if (moved || bot->isMoving())
                {
                    DC_PULL_DEBUG("[DC:{}] follow-tank: skirting room-aggro sphere "
                                  "(r={:.1f}) -> detour ({:.1f}, {:.1f})",
                                  bot->GetName(), safeRadius,
                                  wp->GetPositionX(), wp->GetPositionY());
                    return true;
                }
            }
        }
    }

    // Wider spacing keeps the party out of aggro range while the tank scouts.
    // The tank's breadcrumb trail guides followers at this offset distance.
    float const dist = std::min<float>(sPlayerbotAIConfig.followDistance, 15.0f);

    // Centered trail-follow. Stock Follow() / MoveFollow re-paths to the follow
    // slot through the core PathGenerator, which returns Detour's taut, wall-
    // HUGGING line — so followers scrape walls and clip ledge edges the whole
    // advance, the exact thing PathCenterEnable removes for the TANK. The tank's
    // escort route is already corridor-centered, and its actual footsteps are
    // recorded as breadcrumbs (DcAdvanceAction::RecordBreadcrumb). So once the
    // tank has pulled ahead, walk the follower UP that centered crumb trail
    // instead of re-deriving a parallel wall-hugging path of its own: the
    // centering is inherited for free — the tank paid the navmesh/VMAP cost once
    // when it built the route, and nothing here re-runs CorridorCenter. This is
    // the same mechanism the dynamic scout-lag branch above uses, generalized to
    // the ordinary close-follow. When the follower is already caught up near the
    // tank we DON'T trail — we fall through to the golden-angle Follow() fan,
    // whose spread keeps healers in LOS and the party out of one stack, and over
    // that short a hop wall-hugging is irrelevant. Gated on the same switch as
    // the centering itself (PathCenterEnable): with centering off the tank's
    // crumbs are the wall-hugging line anyway, so there is nothing to inherit
    // and the stock Follow() fan is the right fallback.
    if (DcSettings::GetBool(ObjectGuid::Empty, "PathCenterEnable"))
    {
        float const toTank = bot->GetExactDist2d(tank);
        // Only trail once the tank is beyond the tight follow bubble — i.e. a
        // real corridor traversal is involved, not a fan-out shuffle. Below this
        // the Follow() fan below keeps the cluster tight in healer LOS.
        float const trailEngage = dist + 2.0f;
        if (toTank > trailEngage)
        {
            // Per-bot stagger so the column spreads single-file ALONG the
            // centered trail rather than every follower targeting the one crumb
            // at `dist` behind the tank and piling onto it. Stable per GUID, same
            // spirit as the golden-angle fan below but projected onto the trail.
            uint32 const slot = static_cast<uint32>(bot->GetGUID().GetCounter()) % 4u;
            float const lag = dist + static_cast<float>(slot) * 3.0f;
            Position trailPoint;
            // Skip the trail when the chosen crumb is one we already occupy: re-
            // issuing MoveTo to a point we're basically on micro-steps in place
            // (the scout-lag "two steps forward, two back" dance). Let Follow()
            // take it — it early-outs cleanly when in range.
            if (DcLeaderSignal::GetLeaderScoutTrailPoint(bot, lag, trailPoint) &&
                bot->GetExactDist(&trailPoint) > kTrailArrival)
            {
                // Keep the teardown / orphan-reaper bookkeeping live; the point-
                // move / glide supersedes any MoveFollow a prior tick installed
                // (same as the scout-lag trail / room-aggro skirt branches above —
                // no explicit Stop needed).
                followedTank = tank->GetGUID();
                DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
                // Smooth glide along the centered crumb trail: leave a healthy
                // escort glide alone (re-issue discipline keyed on splineRunning,
                // not the LastMovement wait), then ride the whole crumb window as
                // ONE continuous spline instead of the per-crumb single-point
                // MoveTo below — the per-crumb relaunch is what made followers
                // half-step the whole advance.
                if (TrailSplineRunning(bot))
                    return true;
                if (IssueTrailGlide(botAI, bot, lag))
                {
                    DC_PULL_DEBUG("[DC:{}] follow-tank: gliding tank's centered "
                                  "breadcrumbs ({:.1f}yd behind, lag {:.1f})",
                                  bot->GetName(), toTank, lag);
                    return true;
                }
                // normal_only: never straight-line to a crumb that isn't reachable
                // over a real navmesh path (belt-and-braces — crumbs are already
                // reachability-gated in GetLeaderScoutTrailPoint). Reached only
                // when the glide window was too short / unreachable.
                if (DcMoveTo(bot->GetMapId(), trailPoint.GetPositionX(),
                           trailPoint.GetPositionY(), trailPoint.GetPositionZ(),
                           false, false, /*normal_only=*/true))
                {
                    DC_PULL_DEBUG("[DC:{}] follow-tank: trailing tank's centered "
                                  "breadcrumbs ({:.1f}yd behind, lag {:.1f})",
                                  bot->GetName(), toTank, lag);
                    return true;
                }
            }
            // No usable trail yet (tank just moved off / crumb unreachable): fall
            // through to the stock follow so the party never strands.
        }
    }

    // Remember who we're chasing so the teardown branch above can cancel this
    // continuous MoveFollow once the DC tank goes away.
    followedTank = tank->GetGUID();
    // Record that this player now carries a follow generator so the world-tick
    // orphan reaper can cancel it if the AI is deleted out from under us — a
    // self-bot leaving bot mode never runs the teardown branch above.
    DcFollowerLifecycle::MarkFollowing(bot->GetGUID());
    // Explicit per-bot angle: every follower here is a self-bot (master ==
    // itself), and MovementAction::GetFollowAngle() skips the master while
    // scanning the group — a self-bot never matches its own entry and falls out
    // at 0.0f — so the default Follow() overload gave the WHOLE party the same
    // follow slot and stacked it on one point (which is also what kept the
    // stock collision shuffle permanently armed). Same deterministic
    // golden-angle fan as ComputeCampSlot, so the cluster spreads evenly and
    // each bot's slot never moves between ticks.
    uint32 const seed = static_cast<uint32>(bot->GetGUID().GetCounter());
    float const angle =
        Position::NormalizeOrientation(static_cast<float>(seed) * 2.39996323f);
    return Follow(tank, dist, angle);
}

bool DungeonClearFilterLootAction::Execute(Event /*event*/)
{
    // Same loot-floor enforcement the advance/follow-tank actions run inline
    // while active (see TryLootYield), lifted out so it keeps running while the
    // run is paused. Drop anything already given up from the stock stack/target,
    // proactively skip any in-range corpse/chest below DungeonClear.LootMinQuality
    // (or an un-finishable group-roll, or any chest while IgnoreChests is set),
    // and time out a corpse we've been camped on. All three only prune the stock
    // loot stack/target — no movement here.
    DcLootPolicy::StripSkippedLoot(botAI);
    DcLootPolicy::MaybeSkipUnworthyLoot(botAI);
    DcLootPolicy::MaybeGiveUpCampedLoot(botAI, DC_LOOT_CAMP_TIMEOUT_MS, DC_LOOT_GIVEUP_TTL_MS);
    // Return false: we only removed loot the bot must NOT take. Returning false
    // lets the engine fall through to the stock loot pipeline (open loot 8, move
    // to loot 7, ...) this same tick so whatever survived the filter is still
    // collected. This action sits just above that pipeline (relevance 9) so the
    // prune always runs first.
    return false;
}
bool DungeonClearCampHoldActionBase::Execute(Event /*event*/)
{
    Position camp;
    bool passive = false;
    if (!DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
        return false;

    // Healers hold at camp like everyone else but are pinned with the "stay"
    // strategy instead of "+passive" (ApplyFollowerPassive) so they can heal the
    // tank through the drag-back without RUNNING FORWARD to close heal range —
    // "stay" suppresses playerbots' reach-to-heal movement while leaving the
    // cast-heal action free. The position pin below still applies — we only stop
    // OWNING the tick once they're parked AND someone needs a heal, so the combat
    // engine can run the in-place heal cast.
    bool const isHealer = PlayerbotAI::IsHeal(bot);

    // Go passive (attack nothing) ONLY while the tank is actually tagging (a
    // holding phase). While merely holding at camp between pulls the party stays
    // ready to defend; the reaper strips any DC passive once we leave a holding
    // phase. Idempotent; the matching release is centralized in
    // ReapStrandedPassives so it fires on every exit.
    if (passive)
        DcFollowerLifecycle::ApplyFollowerPassive(bot);

    // Loot yield (scout phase only). The pack dies AT camp, so its corpses sit
    // right where the party holds. Without this the camp hold and the stock loot
    // pipeline fight each other: move-to-loot walks a follower a few yards to a
    // corpse, hold-at-camp yanks it back (corpse just outside the hold radius),
    // and un-finishable / below-floor corpses keep the loot flags armed forever —
    // the ping-pong the player saw. Mirror the follow-tank loot yield exactly:
    // prune corpses we must not take (skipped / below DungeonClear.LootMinQuality /
    // camped too long), then step ASIDE (return false, don't pull back to camp)
    // while a takeable corpse is in progress, bounded by a commit timeout. Skip
    // entirely while the tank is tagging (passive) — the party must stay pinned.
    if (!passive)
    {
        DcLootPolicy::StripSkippedLoot(botAI);
        DcLootPolicy::MaybeSkipUnworthyLoot(botAI);
        DcLootPolicy::MaybeGiveUpCampedLoot(botAI, DC_LOOT_CAMP_TIMEOUT_MS,
                                                DC_LOOT_GIVEUP_TTL_MS);
        uint32& lootYieldStart =
            context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get().lootYieldStartMs;
        bool const lootYield =
            AI_VALUE(bool, DcKey::Stock::HasAvailableLoot) || AI_VALUE(bool, DcKey::Stock::CanLoot);
        if (lootYield)
        {
            uint32 const nowMs = getMSTime();
            if (lootYieldStart == 0)
                lootYieldStart = nowMs;
            if (nowMs - lootYieldStart < DC_LOOT_YIELD_TIMEOUT_MS)
            {
                // Hand the tick to move-to-loot / open-loot; do NOT yank back to
                // camp (that is the ping-pong).
                DC_PULL_TRACE("[DC:{}] hold-at-camp yielding: loot in progress ({}ms)",
                              bot->GetName(), nowMs - lootYieldStart);
                return false;
            }
            // Waited long enough — blacklist THIS corpse so the flags drop and we
            // stop being drawn back to it, then resume holding at camp.
            DcLootPolicy::GiveUpCurrentLoot(botAI, DC_LOOT_GIVEUP_TTL_MS);
            DC_PULL_TRACE("[DC:{}] hold-at-camp loot-yield timed out -> giving up corpse",
                          bot->GetName());
        }
        else
        {
            lootYieldStart = 0;  // not looting -> reset the commit timer
        }
    }

    // Cancel any persistent MoveFollow that follow-tank installed before the
    // pull. While holding we own this bot's movement, but MoveFollow lives in
    // the same ACTIVE MotionMaster slot and StopMoving does NOT remove the
    // generator — it re-asserts on the next motion update and walks the
    // follower right back out after the advancing tank (this is exactly how the
    // party ended up trailing the tank to the mob). Clear it once; follow-tank
    // re-installs it when the pull releases. Same persistent-generator gotcha as
    // the DC-tank-gone teardown / [[selfbot-stale-movefollow]].
    if (bot->GetMotionMaster() &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        DcMovement::StopBot(bot, DcMovement::Stop::Hold);
        context->GetValue<ObjectGuid>(DcKey::FollowedTank)->Set(ObjectGuid::Empty);
        DcFollowerLifecycle::UnmarkFollowing(bot->GetGUID());
    }

    // Held ranged healer: clamp its movement so it can APPROACH the tank to reach
    // heal range but NEVER cross to the threat side of it. The healer carries the
    // "stay" pin (ApplyFollowerPassive) which disables every stock mover — so on
    // the yield below the cast-heal fires but nothing can walk the bot, killing
    // the overshoot where a pure ranged healer ran past the tank to a heal/follow
    // point that sits behind the tank (and behind = toward the pack when the tank
    // faces camp on the drag-back). DC supplies the approach itself, clamped to
    // the camp side of the heal target, so "approach yes, never past the tank".
    if (passive && isHealer)
    {
        Unit* const healTarget = AI_VALUE(Unit*, DcKey::Stock::PartyToHeal);
        uint8 const lowestPct = AI_VALUE2(uint8, DcKey::Stock::Health, DcKey::Stock::PartyToHeal);
        if (healTarget && lowestPct < sPlayerbotAIConfig.almostFullHealth)
        {
            float const healRange = botAI->GetRange("heal");
            bool const canCast = bot->GetExactDist(healTarget) <= healRange &&
                                 bot->IsWithinLOSInMap(healTarget);
            if (canCast)
            {
                // In range + LOS: hold here and YIELD so the (movement-suppressed)
                // cast-heal lands in place. "stay" guarantees no mover runs.
                DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                DC_PULL_TRACE("[DC:{}] healer: in range -> casting in place", bot->GetName());
                return false;
            }
            // Out of range/LOS: drive a clamped approach toward a point at ~85%
            // heal range on the CAMP side of the target. Own the tick (don't let
            // the camp slot yank it straight back) and use MOVEMENT_COMBAT so it
            // wins over any stale combat mover.
            Position const approach =
                DcPullPlanner::ComputeHealApproach(bot, healTarget, camp, healRange);
            if (bot->GetExactDist(&approach) > DC_PULL_SLOT_RADIUS)
            {
                DcMoveTo(bot->GetMapId(), approach.GetPositionX(), approach.GetPositionY(),
                       approach.GetPositionZ(), /*idle*/ false, /*react*/ false,
                       /*normal_only*/ false, /*exact_waypoint*/ false,
                       MovementPriority::MOVEMENT_COMBAT);
                DC_PULL_TRACE("[DC:{}] healer: clamped approach to heal target", bot->GetName());
                return true;
            }
            // At the clamped point but still no LOS/range (corner): hold, yield for
            // a possible cast — never push past the clamp toward the pack.
            DcMovement::StopBot(bot, DcMovement::Stop::Soft);
            return false;
        }
        // Nobody needs healing — fall through to the normal camp-slot pin so the
        // healer holds at / returns to camp like any other held follower.
    }

    // Park at the leader's camp. Each follower aims for its own fuzzed slot — a
    // deterministic 1-2yd offset off the shared anchor, snapped to the navmesh —
    // so the party fans out instead of stacking on one identical point. Settle on
    // the slot with a tight tolerance (the wide hold radius would let the bot stop
    // before the variance ever showed); fall back to the anchor when the slot
    // probe failed (slot == camp).
    Position const slot = DcPullPlanner::ComputeCampSlot(bot, camp);
    float const toCamp = bot->GetExactDist(&slot);
    if (toCamp <= DC_PULL_SLOT_RADIUS)
    {
        DcMovement::StopBot(bot, DcMovement::Stop::Soft);
        // A waiting party watches their tank work: face the LEADER (not the
        // pack) once parked — never while still walking to camp.
        DcFaceIfNeeded(bot, DcLeaderSignal::FindLeaderTank(bot));
        DC_PULL_TRACE("[DC:{}] hold-at-camp: parked ({:.1f}yd, passive={})",
                      bot->GetName(), toCamp, passive);
        // During a holding phase (the tank is tagging) OWN the tick so nothing can
        // break the hold while the pull is live. While merely camped between pulls
        // (scout phase, passive==false) YIELD so the party can rest / loot at camp
        // — the multiplier suppresses wander / follow / self-pull for a camp-held
        // follower, so yielding here can't let it drift off toward the tank. A
        // healer's in-place / clamped-approach healing is handled in the dedicated
        // block above and returns before reaching here; once nobody needs a heal it
        // falls through to this camp pin like any other held follower.
        return passive;
    }
    // Priority matters the moment the party is already IN COMBAT when a pull
    // commits — e.g. a dynamic LEEROY->ADVANCED upgrade that lands after the tank
    // (and the following party) already ran into the pack. During a holding phase
    // (passive) the party MUST retreat to the camp even mid-fight, so move at
    // MOVEMENT_COMBAT — the same priority the tank's drag-back uses — otherwise a
    // MOVEMENT_NORMAL move loses to the stock combat MoveChase generator already
    // installed on the engaged follower and it just DPSes the pack where it stands
    // (the "party didn't run back, chaos" case). While merely scouting between pulls
    // (passive == false, usually out of combat) NORMAL is right: it must NOT stomp
    // the loot/rest pipeline the party runs at camp.
    MovementPriority const prio = passive ? MovementPriority::MOVEMENT_COMBAT
                                          : MovementPriority::MOVEMENT_NORMAL;
    bool const moved =
        DcMoveTo(bot->GetMapId(), slot.GetPositionX(), slot.GetPositionY(), slot.GetPositionZ(),
               /*idle*/ false, /*react*/ false, /*normal_only*/ false,
               /*exact_waypoint*/ false, prio);
    DC_PULL_TRACE("[DC:{}] hold-at-camp: walking to camp ({:.1f}yd, passive={}, moved={})",
                  bot->GetName(), toCamp, passive, moved);
    return true;
}

namespace
{
    // The mob the PARTY is actually fighting, for a follower to assist onto. The old
    // lookup only considered the LEADER's attacker list + victim — which is EMPTY
    // whenever the pack is ranged, or the mobs fixated a DPS instead of the tank, so
    // the tank holds threat with nothing meleeing it. In that (common) case the old
    // code fell back to "move onto the leader", orbiting the tank at 0.0yd doing
    // nothing (proven live: `target=leader` at 0.0yd while assistWanted=1). Resolve
    // from the WHOLE fight instead:
    //   1. anything attacking the leader / the leader's own victim (the held pack), then
    //   2. the nearest hostile any NEARBY in-combat groupmate is fighting (their
    //      attackers + victim) — the mobs that fixated the DPS.
    // Returns the nearest valid, reachable such hostile, or nullptr → the caller
    // STANDS DOWN (never orbits the leader). Members are gated within 1.5x
    // PartyMaxSpread of the tank so this stays "the tank's fight", not a far skirmish.
    Unit* PickPartyFightTarget(Player* bot, Player* leader)
    {
        Unit* best = nullptr;
        float bestDist = 0.0f;
        auto consider = [&](Unit* u)
        {
            if (!u || !u->IsAlive() || u->GetMapId() != bot->GetMapId())
                return;
            if (!bot->IsValidAttackTarget(u))
                return;
            float const d = bot->GetExactDist2d(u);
            if (!best || d < bestDist)
            {
                best = u;
                bestDist = d;
            }
        };
        auto considerFrom = [&](Unit* src)
        {
            if (!src)
                return;
            for (Unit* a : src->getAttackers())
                consider(a);
            consider(src->GetVictim());
        };

        // 1. The leader's own fight takes priority (the pack it is holding).
        considerFrom(leader);
        if (best)
            return best;

        // 2. Else whatever a nearby in-combat groupmate is fighting.
        // When the leader has no combat data of its own (out of combat or
        // between pulls), skip the distance-from-leader filter so ANY
        // fighting groupmate's target resolves — fixing the "groupmate in
        // combat while leader reads out-of-combat" stall.
        bool const leaderFighting = leader && (leader->IsInCombat() ||
            leader->GetVictim() || !leader->getAttackers().empty());
        if (Group* grp = bot->GetGroup())
        {
            float const reach = DcSettings::GetFloat(bot, "PartyMaxSpread") * 1.5f;
            for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (!m || m == bot || !m->IsAlive() || !m->IsInCombat())
                    continue;
                if (m->GetMapId() != bot->GetMapId())
                    continue;
                if (leaderFighting && leader && m->GetExactDist2d(leader) > reach)
                    continue;  // keep it the tank's fight, not a far straggler's
                considerFrom(m);
            }
        }

        // 3. Last resort: the leader's current victim. Steps 1 & 2 scan attackers
        // via consider() which gates on bot->IsValidAttackTarget — that check
        // includes LOS and elevation, so a follower on a lower floor or around a
        // corner can't resolve the mob the tank is actively fighting. Use the
        // leader's victim directly (bypass IsValidAttackTarget) as a movement
        // anchor: once the follower closes distance and gains LOS, the next tick
        // finds a valid target through steps 1 & 2 and proceeds normally.
        if (!best && leader)
        {
            Unit* victim = leader->GetVictim();
            if (victim && victim->IsAlive() && victim->GetMapId() == bot->GetMapId())
                best = victim;
        }

        // 4. If the leader has no victim (AoE tanking with no auto-attack
        // target), pick the closest attacker from the leader's attacker list
        // instead — still bypassing IsValidAttackTarget to handle LOS/elevation.
        if (!best && leader)
        {
            float bestDist2 = 0.0f;
            for (Unit* a : leader->getAttackers())
            {
                if (!a || !a->IsAlive() || a->GetMapId() != bot->GetMapId())
                    continue;
                float const d = bot->GetExactDist2d(a);
                if (!best || d < bestDist2)
                {
                    best = a;
                    bestDist2 = d;
                }
            }
        }

        // 5. Leader still has no combat data (out of combat, groupmate is
        // fighting) — try any in-combat groupmate's victim/attackers.
        if (!best && bot->GetGroup())
        {
            float bestDist2 = 0.0f;
            for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (!m || m == bot || m == leader || !m->IsAlive() || !m->IsInCombat())
                    continue;
                if (m->GetMapId() != bot->GetMapId())
                    continue;
                Unit* vic = m->GetVictim();
                if (vic && vic->IsAlive() && vic->GetMapId() == bot->GetMapId())
                {
                    float const d = bot->GetExactDist2d(vic);
                    if (!best || d < bestDist2) { best = vic; bestDist2 = d; }
                }
                for (Unit* a : m->getAttackers())
                {
                    if (!a || !a->IsAlive() || a->GetMapId() != bot->GetMapId())
                        continue;
                    float const d = bot->GetExactDist2d(a);
                    if (!best || d < bestDist2) { best = a; bestDist2 = d; }
                }
            }
        }

        // 6. Last resort: the bot's own combat data. A rogue taking damage
        // (in combat) with no groupmate combat data still needs a target — use
        // the bot's own victim or the closest attacker hitting it.
        if (!best && bot->IsInCombat())
        {
            if (Unit* vic = bot->GetVictim())
            {
                if (vic->IsAlive() && vic->GetMapId() == bot->GetMapId())
                    best = vic;
            }
            if (!best)
            {
                float bestDist2 = 0.0f;
                for (Unit* a : bot->getAttackers())
                {
                    if (!a || !a->IsAlive() || a->GetMapId() != bot->GetMapId())
                        continue;
                    float const d = bot->GetExactDist2d(a);
                    if (!best || d < bestDist2) { best = a; bestDist2 = d; }
                }
                if (best)
                    DC_PULL_DEBUG("[DC:{}] assist: no groupmate target, using own attacker {}", bot->GetName(), best->GetGUID().ToString());
            }
        }

        // 7. Nothing from anyone's attacker/victim lists — the mob has aggro
        // on a party member but hasn't dealt damage yet (still pathing in), so
        // it never entered any getAttackers() set. Scan for the nearest hostile
        // near the closest in-combat groupmate and target that instead.
        if (!best && bot->GetGroup())
        {
            for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (!m || m == bot || !m->IsAlive() || !m->IsInCombat() || m->GetMapId() != bot->GetMapId())
                    continue;
                // Grid-scan all creatures within 15yd of the in-combat groupmate.
                // entry=0 means any creature; the range check is against the
                // groupmate's position, so we scan from the bot and verify distance
                // to the groupmate below.
                std::list<Creature*> nearby;
                bot->GetCreatureListWithEntryInGrid(nearby, 0, 25.0f);
                float closestDist = 25.0f;
                Creature* match = nullptr;
                for (Creature* c : nearby)
                {
                    if (!c || !c->IsAlive() || c->IsFriendlyTo(bot) || !bot->IsValidAttackTarget(c))
                        continue;
                    if (m->GetExactDist2d(c) > 15.0f)
                        continue;
                    float const d = bot->GetExactDist2d(c);
                    if (!match || d < closestDist)
                    {
                        match = c;
                        closestDist = d;
                    }
                }
                if (match)
                {
                    DC_PULL_DEBUG("[DC:{}] assist: no attacker found, scanning near in-combat groupmate -> {} ({:.1f}yd)",
                                  bot->GetName(), match->GetGUID().ToString(), bot->GetExactDist2d(match));
                    best = match;
                    break;
                }
            }
        }

        return best;
    }
}

bool DungeonClearAssistCampActionBase::Execute(Event /*event*/)
{
    Player* leader = DcLeaderSignal::FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;

    // Resolve a REAL mob the party is fighting from the WHOLE group (see
    // PickPartyFightTarget). Falls back to the leader's current victim if no
    // valid attack target resolves (covers LOS/elevation gaps).
    Unit* target = PickPartyFightTarget(bot, leader);
    if (!target)
    {
        // Fallback: move toward the nearest in-combat party member so the
        // follower at least closes the gap instead of standing idle.
        // Covers underwater combat, brief attacker-list gaps, and neutral
        // mobs where getAttackers() resolves late relative to IsInCombat().
        if (Group* g = bot->GetGroup())
        {
            Player* nearest = nullptr;
            float nearestDist = 0.0f;
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (!m || m == bot || !m->IsAlive() || m->GetMapId() != bot->GetMapId() || !m->IsInCombat())
                    continue;
                float const d = bot->GetExactDist2d(m);
                if (!nearest || d < nearestDist) { nearest = m; nearestDist = d; }
            }
            if (nearest)
            {
                DC_PULL_DEBUG("[DC:{}] assist camp: no fight target — closing on {} ({:.1f}yd)",
                               bot->GetName(), nearest->GetName(), nearestDist);
                DcMoveTo(nearest->GetMapId(), nearest->GetPositionX(), nearest->GetPositionY(),
                       nearest->GetPositionZ(), false, false, false, false,
                       MovementPriority::MOVEMENT_COMBAT);
                return true;
            }
        }
        DC_PULL_DEBUG("[DC:{}] assist camp: no fight target (vic={} atk={})",
                      bot->GetName(),
                      leader->GetVictim() ? leader->GetVictim()->GetGUID().ToString().c_str() : "null",
                      leader->getAttackers().empty() ? 0u : static_cast<uint32>(leader->getAttackers().size()));
        return false;
    }

    // Seed the fight target so BOTH engines have a valid "current target": select it,
    // publish it, and open a combat window so the bot is in the fight the instant it
    // reaches range/sight. This target is on the TANK — never on this bot's own
    // attacker list — so without seeding, stock target-acquisition finds nothing
    // while the pack is out of sight.
    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(target);
    if (!bot->IsInCombat())
        bot->SetInCombatWith(target);

    // This action is registered under two names — "...assist camp combat" runs in the
    // COMBAT engine, "...assist camp" in the NON-combat engine. Do NOT key this off
    // bot->IsInCombat(): the whole bug is the limbo where the bot is combat-FLAGGED
    // yet still ticking the non-combat engine (no target of its own), so IsInCombat
    // lies about the engine.
    bool const inCombatEngine = getName().find("combat") != std::string::npos;

    // NON-combat side (the limbo): flip into the combat engine IMMEDIATELY — do NOT
    // wait for LOS/range. Once there, the bot's rotation runs and the combat-side
    // assist below drives the approach. The DC drop-target suppressor
    // (DungeonClearMultiplier) keeps the out-of-LOS seed from bouncing the bot right
    // back out (drop target rel 99 >> reach rel 20 would win every tick otherwise).
    // This is the flip-early model: the party enters combat WITH the tank and drives
    // on the mob, not on the human/tank.
    if (!inCombatEngine)
    {
        botAI->ChangeEngine(BOT_STATE_COMBAT);
        DC_PULL_TRACE("[DC:{}] assist camp: seeded {} -> flip to combat engine",
                      bot->GetName(), target->GetGUID().ToString());
        return true;
    }

    // --- COMBAT engine ---------------------------------------------------------
    float const dist = bot->GetExactDist(target);
    float const range = botAI->IsMelee(bot)
        ? (bot->GetCombatReach() + target->GetCombatReach() + 1.0f)
        : (botAI->GetRange("spell") - CONTACT_DISTANCE);

    // HAVE LINE OF SIGHT -> hand positioning to STOCK combat; do NOT drive our own
    // move. Stock "reach spell" stops a ranged bot at its spell range (it goes inert
    // once inside range), and "reach melee" closes a melee bot to melee — the correct
    // per-class standoff. Driving DcMoveTo toward the mob here is exactly what marched
    // ranged DPS into melee even with a clear shot (the reported bug): our move aims
    // at the mob's feet and only our own in-range test could stop it. So:
    //   * in range: ENGAGE (face + Attack commits the victim so the rotation fires —
    //     SetInCombatWith alone never swings/casts), then YIELD (return false) so the
    //     rotation runs THIS tick; returning true would starve it at rel 35.
    //   * out of range: just YIELD — stock reach spell/melee (rel 20) closes to the
    //     right distance and stops. We re-arm next tick until in range.
    if (bot->IsWithinLOSInMap(target))
    {
        if (dist <= range)
        {
            if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
                ServerFacade::instance().SetFacingTo(bot, target);
            bot->Attack(target, botAI->IsMelee(bot));
            DC_PULL_TRACE("[DC:{}] assist camp: engaged {} in range+LOS ({:.1f}yd) -> yield",
                          bot->GetName(), target->GetGUID().ToString(), dist);
        }
        else
        {
            DC_PULL_TRACE("[DC:{}] assist camp: LOS, out of range ({:.1f}yd) -> yield to "
                          "stock reach", bot->GetName(), dist);
        }
        return false;
    }

    // NO LINE OF SIGHT: close on the MOB (never the tank) to round the corner and
    // regain sight — the one case stock combat can't handle (its reach is distance-
    // only). Band the approach exactly as EngageDirect does — COMBAT priority can't be
    // interrupted by the bot's combat reflexes, so on a LONG assist run an
    // unconditional COMBAT move plows through every pack it aggros en route. COMBAT
    // only for the final close approach; past that NORMAL, so the follower stops and
    // fights what it pulls on the way in.
    MovementPriority const prio =
        (dist <= range + DC_COMBAT_APPROACH_RANGE)
            ? MovementPriority::MOVEMENT_COMBAT
            : MovementPriority::MOVEMENT_NORMAL;

    DC_PULL_TRACE("[DC:{}] assist camp: no-LOS, closing on mob {} ({:.1f}yd, prio={})",
                  bot->GetName(), target->GetGUID().ToString(), dist,
                  prio == MovementPriority::MOVEMENT_COMBAT ? "combat" : "normal");

    DcMoveTo(target->GetMapId(), target->GetPositionX(), target->GetPositionY(),
           target->GetPositionZ(), /*idle*/ false, /*react*/ false,
           /*normal_only*/ false, /*exact_waypoint*/ false, prio);
    return true;
}

bool DungeonClearRegroupCombatAction::Execute(Event /*event*/)
{
    // The leader tank, non-null only on an active run (gated again by the trigger).
    Player* tank = AI_VALUE(Player*, DcKey::PartyTank);
    if (!tank || tank == bot)
        return false;

    Map* map = bot->GetMap();
    if (!map)
        return false;

    bool const isHealer = PlayerbotAI::IsHeal(bot);
    bool const isMelee  = botAI->IsMelee(bot);

    // Anchor the reconnect. A healer PRE-POSITIONS on the tank (the goal is "be able
    // to heal the tank the moment damage starts", so the tank IS the anchor); a DPS
    // anchors on the fight the tank is holding, so it regains LOS on a target, not on
    // the tank's back. LeaderFightAnchor writes the tank's own position when no
    // concrete fight unit resolves, so anchorPos is always usable.
    Position anchorPos;
    Unit* anchorUnit = nullptr;
    if (isHealer)
        anchorPos = tank->GetPosition();
    else
        anchorUnit = DcTargeting::LeaderFightAnchor(bot, tank, anchorPos);

    // Role-range standoff ring. Ranged DPS: 0.8x spell range (inside range so target
    // wobble doesn't drop it back out). Healer: max(5, 0.6x heal range) — the same
    // band HealReposition parks a healer in, so the two never disagree. Melee has no
    // ring: it closes on the anchor (rounding the corner is what regains LOS), so it
    // falls through to the fractional approach below.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool sampled = false;
    if (!isMelee)
    {
        float ringRadius = 0.0f;
        float maxRadius = 0.0f;
        if (isHealer)
        {
            float const healRange = botAI->GetRange("heal");
            maxRadius = healRange;
            ringRadius = std::max(5.0f, healRange * 0.6f);
        }
        else
        {
            float const spellRange = botAI->GetRange("spell");
            maxRadius = spellRange;
            ringRadius = spellRange * 0.8f;
        }
        sampled = FindStandoffPoint(map, anchorPos, ringRadius, maxRadius, x, y, z);
    }

    // Fallback: no ring point validated (tight geometry, snap misses), or a melee.
    // Close on the anchor with pathfinding, stopping attackRange short so a ranged
    // class doesn't pile into the melee/AoE and a melee lands in swing range. The
    // predicate clears the instant a mob comes into sight, so this only ever walks
    // far enough to round the corner.
    if (!sampled)
    {
        float const ax = anchorPos.GetPositionX();
        float const ay = anchorPos.GetPositionY();
        float const az = anchorPos.GetPositionZ();
        float const attackRange = isMelee
            ? (bot->GetCombatReach() + (anchorUnit ? anchorUnit->GetCombatReach() : 0.0f) + 1.0f)
            : std::max(5.0f, botAI->GetRange("spell") - CONTACT_DISTANCE);
        float const dist = bot->GetExactDist2d(ax, ay);
        x = ax;
        y = ay;
        z = az;
        if (dist > attackRange)
        {
            float const frac = (dist - attackRange) / dist;
            x = bot->GetPositionX() + (ax - bot->GetPositionX()) * frac;
            y = bot->GetPositionY() + (ay - bot->GetPositionY()) * frac;
        }
    }

    // Re-issue guard: the trigger latches and re-fires every tick, but re-plotting a
    // near-identical spline each time stutters and clips casts (cf. spline-reissue
    // freeze). While already travelling toward within 3yd of the same point, own the
    // tick without touching the move.
    if (bot->isMoving() && _lastDestValid &&
        _lastDest.GetExactDist2d(x, y) < 3.0f)
        return true;

    // Band the priority like EngageDirect / the camp assist: COMBAT only for the
    // final close approach, NORMAL beyond. An unconditional COMBAT regroup runs a
    // stranded follower straight through any packs between it and the anchor without
    // stopping to fight — the plow-through runaway. NORMAL on the long leg lets it
    // break off and clear what it aggros, then resume once that mob dies.
    float const toDest = bot->GetExactDist2d(x, y);
    MovementPriority const prio =
        (toDest <= DC_COMBAT_APPROACH_RANGE)
            ? MovementPriority::MOVEMENT_COMBAT
            : MovementPriority::MOVEMENT_NORMAL;

    DC_PULL_TRACE("[DC:{}] regroup: moving to standoff ({:.1f}yd, healer={}, "
                  "sampled={}, anchor={}, prio={})",
                  bot->GetName(), toDest, isHealer ? 1 : 0, sampled ? 1 : 0,
                  anchorUnit ? anchorUnit->GetGUID().ToString() : "tank",
                  prio == MovementPriority::MOVEMENT_COMBAT ? "combat" : "normal");

    if (DcMoveTo(map->GetId(), x, y, z, /*idle*/ false, /*react*/ false,
                 /*normal_only*/ false, /*exact_waypoint*/ false, prio))
    {
        _lastDest = Position(x, y, z, 0.0f);
        _lastDestValid = true;
    }
    return true;
}

bool DungeonClearHealRepositionAction::Execute(Event /*event*/)
{
    // The most-hurt heal target (LOS-blind, tank-biased). Stored as a GUID (like
    // the pull target), resolved live here. Re-read (trigger/action gap); bail if
    // it healed up or died in between.
    ObjectGuid const targetGuid = AI_VALUE(ObjectGuid, DcKey::HealTarget);
    if (targetGuid.IsEmpty())
        return false;
    Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid);
    if (!target || !target->IsAlive())
        return false;

    Map* map = bot->GetMap();
    if (!map)
        return false;

    float const healRange = botAI->GetRange("heal");
    Position const targetPos = target->GetPosition();
    float const tx = targetPos.GetPositionX();
    float const ty = targetPos.GetPositionY();
    float const tz = targetPos.GetPositionZ();

    // Stand a little INSIDE heal range so a step of target movement doesn't drop us
    // straight back out. Floor at 5yd so we never try to stack on the target.
    float const standoff = std::max(5.0f, healRange * 0.6f);

    // First ring point around the target that snaps, sits within heal range, has
    // LOS, and is reachable (shared with the combat regroup — see FindStandoffPoint).
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    bool const haveDest = FindStandoffPoint(map, targetPos, standoff, healRange, dx, dy, dz);

    // Fallback: no sampled point validated (tight geometry, snap misses). Close
    // straight on the target with pathfinding — rounding corners is what regains
    // LOS — stopping 5yd short, exactly the old regroup behaviour.
    if (!haveDest)
    {
        float const dist = bot->GetExactDist2d(target);
        dx = tx;
        dy = ty;
        dz = tz;
        if (dist > 5.0f)
        {
            float const frac = (dist - 5.0f) / dist;
            dx = bot->GetPositionX() + (tx - bot->GetPositionX()) * frac;
            dy = bot->GetPositionY() + (ty - bot->GetPositionY()) * frac;
        }
    }

    // Band the priority like the assist/regroup actions: COMBAT only on the final
    // close leg, NORMAL beyond, so a long run back stops to fight what it aggros
    // instead of plowing a mob train to the target.
    float const toDest = bot->GetExactDist2d(dx, dy);
    MovementPriority const prio =
        (toDest <= DC_COMBAT_APPROACH_RANGE)
            ? MovementPriority::MOVEMENT_COMBAT
            : MovementPriority::MOVEMENT_NORMAL;

    DC_PULL_TRACE("[DC:{}] heal reposition: closing on heal target {} "
                  "({:.1f}yd, los={}, sampled={}, prio={})",
                  bot->GetName(), target->GetGUID().ToString(),
                  bot->GetExactDist2d(target),
                  bot->IsWithinLOSInMap(target) ? 1 : 0, haveDest ? 1 : 0,
                  prio == MovementPriority::MOVEMENT_COMBAT ? "combat" : "normal");

    DcMoveTo(map->GetId(), dx, dy, dz, /*idle*/ false, /*react*/ false,
           /*normal_only*/ false, /*exact_waypoint*/ false, prio);
    return true;
}

bool DungeonClearLeaderAssistAction::Execute(Event /*event*/)
{
    // Leader-side assist. The trigger guarantees: this bot IS the leader, it is out
    // of combat with no visible target of its own, and a groupmate is (latched) in
    // combat with a pack the tank never saw. Find what the party is fighting, take
    // threat, and move onto it. The inverse of the follower assist, which finds
    // what the LEADER is fighting and drives the follower to it.
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    // Nearest hostile attacking an in-combat groupmate (the pack a follower pulled
    // around the corner), plus the nearest in-combat member as a fallback move-to
    // so the tank at least rounds the corner back into sight when no concrete
    // attacker resolves (brief threat-table gap). LOS-blind on purpose: the whole
    // point is the fight the tank's own sight-gated picker can't reach.
    Unit* target = nullptr;
    float bestTargetDist = 0.0f;
    Player* nearestFighter = nullptr;
    float bestFighterDist = 0.0f;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive())
            continue;
        if (member->GetMapId() != bot->GetMapId() || !member->IsInCombat())
            continue;

        float const md = bot->GetExactDist2d(member);
        if (!nearestFighter || md < bestFighterDist)
        {
            nearestFighter = member;
            bestFighterDist = md;
        }

        // Everything meleeing this groupmate, plus its own victim — the pack we
        // must peel onto the tank.
        for (Unit* a : member->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetMapId() != bot->GetMapId())
                continue;
            if (!bot->IsValidAttackTarget(a))
                continue;
            float const d = bot->GetExactDist2d(a);
            if (!target || d < bestTargetDist)
            {
                target = a;
                bestTargetDist = d;
            }
        }
        if (!target)
        {
            Unit* const victim = member->GetVictim();
            if (victim && victim->IsAlive() && victim->GetMapId() == bot->GetMapId() &&
                bot->IsValidAttackTarget(victim))
            {
                target = victim;
                bestTargetDist = bot->GetExactDist2d(victim);
            }
        }
    }

    // No concrete target and nobody resolvable to walk toward — fall back to
    // moving toward the nearest party member on this map. A member taking
    // damage from a neutral mob may not have IsInCombat() set yet, and
    // getAttackers() can be empty (underwater BFD). Just closing distance
    // rounds the corner and lets the tank's own LOS-gated picker take over.
    if (!target && !nearestFighter)
    {
        Player* fallback = nullptr;
        float fallbackDist = 0.0f;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || m == bot || !m->IsAlive() || m->GetMapId() != bot->GetMapId())
                continue;
            float const d = bot->GetExactDist2d(m);
            if (!fallback || d < fallbackDist)
            {
                fallback = m;
                fallbackDist = d;
            }
        }
        if (!fallback)
            return false;
        DC_PULL_DEBUG("[DC:{}] leader assist: no combat data — closing on {} ({:.1f}yd)",
                       bot->GetName(), fallback->GetName(), fallbackDist);
        DcMoveTo(fallback->GetMapId(), fallback->GetPositionX(), fallback->GetPositionY(),
               fallback->GetPositionZ(), false, false, false, false,
               MovementPriority::MOVEMENT_NORMAL);
        return true;
    }

    Unit* const moveTo = target ? target : static_cast<Unit*>(nearestFighter);

    // Take threat ONLY once the pack is in sight. Force-combating while still out of
    // LOS would flip the tank into its combat engine next tick — where THIS
    // (non-combat) trigger goes inert and the approach would stall mid-corner — and
    // stock combat can't reliably chase a target it can't see. So while out of LOS
    // we just keep walking toward the fight (the trigger re-fires every tick and
    // drives us on); the instant we round the corner into sight we commit, and stock
    // combat / the tank rotation close the final gap and hold the pack.
    if (target && bot->IsWithinLOSInMap(target))
    {
        bot->SetSelection(target->GetGUID());
        context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(target);
        if (!bot->IsInCombat())
            bot->SetInCombatWith(target);
        DC_PULL_TRACE("[DC:{}] leader assist: in sight of party fight ({:.1f}yd) "
                      "-> took threat, combat engine takes over",
                      bot->GetName(), bot->GetExactDist(target));
        return true;
    }

    // Movement priority: when the tank is ALREADY in combat (peeling a loose mob
    // while fighting a pack), use MOVEMENT_COMBAT so the approach overrides the
    // combat engine's MoveChase generator — otherwise the tank stays glued to its
    // current target and never peels. When out of combat (running to a groupmate
    // who pulled around a corner), NORMAL lets the tank fight anything it pulls
    // en route so it doesn't plow a mob train to the fight.
    MovementPriority const prio = bot->IsInCombat()
        ? MovementPriority::MOVEMENT_COMBAT
        : [&]() -> MovementPriority
          {
              float const distance2 = bot->GetExactDist(moveTo);
              float const attackRange2 = botAI->IsMelee(bot)
                  ? (bot->GetCombatReach() + moveTo->GetCombatReach() + 1.0f)
                  : (botAI->GetRange("spell") - CONTACT_DISTANCE);
              return (distance2 <= attackRange2 + DC_COMBAT_APPROACH_RANGE)
                  ? MovementPriority::MOVEMENT_COMBAT
                  : MovementPriority::MOVEMENT_NORMAL;
          }();

    DC_PULL_TRACE("[DC:{}] leader assist: closing on party fight ({:.1f}yd, "
                   "target={}, prio={})", bot->GetName(), bot->GetExactDist(moveTo),
                  target ? target->GetGUID().ToString() : "groupmate",
                  prio == MovementPriority::MOVEMENT_COMBAT ? "combat" : "normal");

    DcMoveTo(moveTo->GetMapId(), moveTo->GetPositionX(), moveTo->GetPositionY(),
           moveTo->GetPositionZ(), /*idle*/ false, /*react*/ false,
           /*normal_only*/ false, /*exact_waypoint*/ false, prio);
    return true;
}
