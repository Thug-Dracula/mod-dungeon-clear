/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"

#include <algorithm>

#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

namespace DcMovement
{
    // Zero the escort spline's LastMovement wait. When the spline was issued,
    // LastMovement was Set with a delay sized to the window travel time (capped
    // at maxWaitForMove, up to ~5s). The advance re-issue guard early-outs while
    // IsWaitingForLastMove() is true, so after a glide is halted the bot would
    // otherwise idle for the remainder of that delay before re-issuing. Zeroing
    // lastdelayTime makes IsWaitingForLastMove() false immediately.
    static void ZeroLastMovementWait(Player* bot)
    {
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (AiObjectContext* ctx = botAI->GetAiObjectContext())
                ctx->GetValue<LastMovement&>("last movement")->Get().lastdelayTime = 0.0f;
    }

    // Kill an in-flight ESCORT glide so a forced rebuild/reset starts from a
    // standstill. Without this a now-stale EscortMovementGenerator keeps driving
    // the bot down the OLD route while the rebuilt path is ignored. Only touches
    // our own escort glide; no-op otherwise.
    static void StopActiveSplineGlide(Player* bot)
    {
        if (!bot)
            return;
        MotionMaster* mm = bot->GetMotionMaster();
        if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE)
            bot->StopMoving();
        ZeroLastMovementWait(bot);
    }

    void ResolveEscortConflict(Player* bot)
    {
        if (!bot)
            return;
        MotionMaster* mm = bot->GetMotionMaster();
        // Only act when an escort glide is actually in flight, so a point-move at
        // a site with no glide is not perturbed (in particular the LastMovement
        // wait is left untouched there).
        if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE)
            StopActiveSplineGlide(bot);
    }

    void StopBot(Player* bot, Stop strength)
    {
        if (!bot)
            return;
        MotionMaster* mm = bot->GetMotionMaster();

        switch (strength)
        {
            case Stop::Soft:
                if (bot->isMoving())
                    bot->StopMoving();
                return;

            case Stop::Hold:
            {
                // Follower path: tear down a persistent FOLLOW generator. A
                // self-bot cannot self-heal a leftover follow (its own follow
                // no-ops without clearing), so any hold must clear it explicitly.
                if (mm && mm->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                {
                    if (bot->isMoving())
                        bot->StopMoving();
                    mm->Clear();
                }

                bool const escortGlide =
                    mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE;
                if (!escortGlide && !bot->isMoving())
                    return;
                StopActiveSplineGlide(bot);
                bot->StopMovingOnCurrentPos();
                return;
            }

            case Stop::HardPin:
                // No early-out: under CC the bot is not "moving" yet a queued
                // MOVEMENT_COMBAT MoveTo lurks under the CC generator and would
                // resume the moment the impairment clears.
                StopActiveSplineGlide(bot);
                bot->StopMovingOnCurrentPos();
                return;
        }
    }

    bool DcMovementAllowed(PlayerbotAI* botAI)
    {
        if (!botAI)
            return false;
        AiObjectContext* ctx = botAI->GetAiObjectContext();
        if (!ctx)
            return false;
        return !ctx->GetValue<bool>("dungeon clear paused")->Get();
    }

    bool SplinePath(PlayerbotAI* botAI, Movement::PointsArray& pts,
                    MovementPriority recordPrio)
    {
        if (!botAI)
            return false;
        Player* bot = botAI->GetBot();
        if (!bot || pts.size() < 2)
            return false;
        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm)
            return false;

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        if (bot->IsNonMeleeSpellCast(true))
        {
            bot->CastStop();
            botAI->InterruptSpell();
        }

        mm->MoveSplinePath(&pts, FORCED_MOVEMENT_NONE);

        // Record movement so AttackAction::Attack still clears the spline when a
        // patrol aggros mid-glide (its interrupt gate is priority <
        // MOVEMENT_COMBAT). The re-issue guards key off splineRunning, not this
        // delay, so its only remaining job is priority arbitration; sizing it to
        // the window travel time keeps it a faithful "this move lasts ~this long"
        // for the framework's other movement consumers without gating our re-issue.
        float windowLen = 0.0f;
        for (size_t i = 1; i < pts.size(); ++i)
            windowLen += (pts[i] - pts[i - 1]).magnitude();
        float const runSpeed = std::max(0.1f, bot->GetSpeed(MOVE_RUN));
        float delay = 1000.0f * (windowLen / runSpeed);
        delay = std::min(delay, static_cast<float>(sPlayerbotAIConfig.maxWaitForMove));
        delay = std::max(delay, static_cast<float>(sPlayerbotAIConfig.reactDelay));

        G3D::Vector3 const& dest = pts.back();
        botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")
            ->Get().Set(bot->GetMapId(), dest.x, dest.y, dest.z, bot->GetOrientation(),
                        delay, recordPrio);

        // The cadence of these lines IS the step-pause signature: consecutive
        // issues spaced ~= their own `delay` mean seamless chaining; a gap much
        // larger than `delay` between a short window's issue and the next is the
        // dead-pause to investigate.
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] spline issued: {} pts, {:.1f}yd, speed={:.1f}, delay={:.0f}ms",
                  bot->GetName(), pts.size(), windowLen, runSpeed, delay);
        return true;
    }
}
