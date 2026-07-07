/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcStrategyGate.h"

#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFollowerLifecycle.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

namespace
{
    char const* const kNonCombat = "dungeon clear";
    char const* const kCombat    = "dungeon clear combat";

    // Strip-time cleanup. A bot that is losing the DC strategies must not carry
    // any live run state past the triggers that owned it:
    //   * a LEADER's `dungeon clear enabled` flag would otherwise survive on its
    //     value context and auto-resume the clear the next time it enters a
    //     dungeon and the strategy is re-installed. DisableDungeonClear resets the
    //     whole run (flags + approach/pull FSMs + long-path cache) in lockstep.
    //   * a FOLLOWER's persistent MoveFollow generator (installed by the
    //     follow-tank action) is never self-healed by a self-bot — see the
    //     selfbot-stale-movefollow note. Clear it explicitly, mirroring the
    //     follow-tank teardown tick.
    // Both are gated so the common case (a bot that never ran DC) does no work and
    // emits no addon chatter.
    void TeardownOnStrip(PlayerbotAI* botAI, Player* bot)
    {
        AiObjectContext* ctx = botAI->GetAiObjectContext();

        if (ctx->GetValue<bool>(DcKey::Enabled)->Get())
            DcActionShared::DisableDungeonClear(
                botAI, "Left the dungeon \xe2\x80\x94 dungeon clear disabled.");

        ObjectGuid& followed =
            ctx->GetValue<ObjectGuid>(DcKey::FollowedTank)->RefGet();
        if (!followed.IsEmpty())
        {
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            followed = ObjectGuid::Empty;
            DcFollowerLifecycle::UnmarkFollowing(bot->GetGUID());
        }
    }
}

namespace DcStrategyGate
{
    void Reconcile(Player* bot)
    {
        if (!bot)
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;  // real player (no bot AI) — nothing to gate

        Map* map = bot->GetMap();
        bool const inDungeon = map && map->IsDungeon();

        bool const hasNon = botAI->HasStrategy(kNonCombat, BOT_STATE_NON_COMBAT);
        bool const hasCmb = botAI->HasStrategy(kCombat, BOT_STATE_COMBAT);

        // Per-engine decision via the pure kernel. The two engines are installed
        // and stripped together, but each is checked independently so a partial
        // state (e.g. a reset that rebuilt only one engine) self-heals.
        Action const nonAction = Decide(inDungeon, hasNon);
        Action const cmbAction = Decide(inDungeon, hasCmb);

        if (nonAction == Action::None && cmbAction == Action::None)
            return;  // already compliant — the hot path

        // Run the strip cleanup once, before removing either engine's strategy,
        // so the run state is torn down while its values/actions still exist.
        if (nonAction == Action::Strip || cmbAction == Action::Strip)
            TeardownOnStrip(botAI, bot);

        switch (nonAction)
        {
            case Action::Install: botAI->ChangeStrategy("+dungeon clear", BOT_STATE_NON_COMBAT); break;
            case Action::Strip:   botAI->ChangeStrategy("-dungeon clear", BOT_STATE_NON_COMBAT); break;
            case Action::None:    break;
        }
        switch (cmbAction)
        {
            case Action::Install: botAI->ChangeStrategy("+dungeon clear combat", BOT_STATE_COMBAT); break;
            case Action::Strip:   botAI->ChangeStrategy("-dungeon clear combat", BOT_STATE_COMBAT); break;
            case Action::None:    break;
        }
    }

    void ReconcileAllBots()
    {
        // Iterate every online player and reconcile the ones that are bots.
        // Reconcile() no-ops on real players (no bot AI) and on already-compliant
        // bots, so this is cheap. Runs on the world thread inside World::Update,
        // the same thread that adds/removes players, so the container is stable
        // for the duration of the loop.
        for (auto const& kv : ObjectAccessor::GetPlayers())
            Reconcile(kv.second);
    }
}
