/*
 * mod-dungeon-clear — DungeonClearModule.cpp
 *
 * Drop-in glue against STOCK mod-playerbots. Two scripts:
 *
 *  1. WorldScript — on the first world-update tick (guaranteed after
 *     playerbots' OnBeforeWorldInitialized -> PlayerbotAIConfig::Initialize ->
 *     AiObjectContext::BuildAllSharedContexts has built every shared context),
 *     append the four DungeonClear contexts into the engine's shared
 *     registries. Doing it on the first tick (not in our own
 *     OnBeforeWorldInitialized) sidesteps any module hook-ordering race.
 *
 *     CRITICAL: playerbots does NOT use one global registry. Each bot is built
 *     by AiFactory from a PER-CLASS context (WarriorAiObjectContext, …), and
 *     each of those declares its OWN shared static lists. The base
 *     AiObjectContext lists are only the fallback for class 0 (no real bot).
 *     So we must append into all ten class registries — appending into the
 *     base alone reaches no bot (that was the "dc on: unknown action" bug).
 *     The per-class lists are PUBLIC statics, so we touch them directly; only
 *     the base lists are private, reached via AiObjectContextAccess.h.
 *
 *  2. PlayerScript — on login, apply the single "dungeon clear" strategy to a
 *     bot's non-combat engine. It bundles the party-chat keyword listener
 *     (`dc on`, …), the driving advance/engage/stall ladder, and the
 *     follow-tank trigger. Resident on ALL bots but inert unless that bot's own
 *     "dungeon clear enabled" flag is set — every driving trigger and the
 *     multiplier gate on the flag, and follow-tank no-ops on the tank itself.
 *     Keeping it on every bot (mirroring the AiFactory add stock playerbots
 *     used to carry) is what lets non-tank party bots redirect their follow
 *     target to the tank while a tank has DC enabled, and revert to the player
 *     when it's off — driven purely by the tank's flag (see
 *     DungeonClearPartyTankValue / DungeonClearFollowTankTrigger).
 *
 *     This login hook is a zero-config convenience for bots present at login.
 *     The reliable universal path — and the ONLY one that reaches a self-bot
 *     created mid-session via `.playerbots bot self` — is the playerbots config
 *     `NonCombatStrategies = "+dungeon clear"`, applied when each bot's engines
 *     are built. The `.dc` slash command (DungeonClearCommand.cpp) needs
 *     neither, since it dispatches the action directly.
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "Player.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "AiObjectContextAccess.h"

// Per-class context registries — each owns its own shared static lists.
#include "DKAiObjectContext.h"
#include "DruidAiObjectContext.h"
#include "HunterAiObjectContext.h"
#include "MageAiObjectContext.h"
#include "PaladinAiObjectContext.h"
#include "PriestAiObjectContext.h"
#include "RogueAiObjectContext.h"
#include "ShamanAiObjectContext.h"
#include "WarlockAiObjectContext.h"
#include "WarriorAiObjectContext.h"

#include "Ai/Dungeon/DungeonClear/DungeonClearActionContext.h"
#include "Ai/Dungeon/DungeonClear/DungeonClearStrategyContext.h"
#include "Ai/Dungeon/DungeonClear/DungeonClearTriggerContext.h"
#include "Ai/Dungeon/DungeonClear/DungeonClearValueContext.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPathWorker.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"

namespace
{
    // Completed-but-uncollected async path results older than this are swept
    // from DcPathWorker's mailbox. Far longer than a normal poll cadence (the
    // owning bot drains its result on its very next AI tick), so this only ever
    // catches results orphaned by a logout / dc-off mid-build.
    constexpr uint32 DC_ASYNC_PATH_RESULT_TTL_MS = 30 * 1000;
}

namespace
{
    // Append a fresh set of DungeonClear contexts into one class's public
    // shared static lists. Each context carries its own creator map, merged
    // into that list's `creators` on Add(); every per-bot NamedObjectContextList
    // holds those by reference, so existing and future bots of this class see
    // the new actions/strategies/triggers/values immediately.
    template <class Ctx>
    void RegisterClassContexts()
    {
        Ctx::sharedStrategyContexts.Add(new DungeonClearStrategyContext());
        Ctx::sharedActionContexts.Add(new DungeonClearActionContext());
        Ctx::sharedTriggerContexts.Add(new DungeonClearTriggerContext());
        Ctx::sharedValueContexts.Add(new DungeonClearValueContext());
    }
}

class DungeonClearRegistrarWorldScript : public WorldScript
{
public:
    DungeonClearRegistrarWorldScript()
        : WorldScript("DungeonClearRegistrarWorldScript") {}

    void OnUpdate(uint32 /*diff*/) override
    {
        if (_registered)
            return;
        _registered = true;

        // All ten class registries — these are what real bots actually use.
        RegisterClassContexts<WarriorAiObjectContext>();
        RegisterClassContexts<PaladinAiObjectContext>();
        RegisterClassContexts<DruidAiObjectContext>();
        RegisterClassContexts<DKAiObjectContext>();
        RegisterClassContexts<HunterAiObjectContext>();
        RegisterClassContexts<MageAiObjectContext>();
        RegisterClassContexts<PriestAiObjectContext>();
        RegisterClassContexts<RogueAiObjectContext>();
        RegisterClassContexts<ShamanAiObjectContext>();
        RegisterClassContexts<WarlockAiObjectContext>();

        // Base registry — only the class-0 fallback uses it, kept for parity.
        dc_access::SharedStrategyContexts()->Add(new DungeonClearStrategyContext());
        dc_access::SharedActionContexts()->Add(new DungeonClearActionContext());
        dc_access::SharedTriggerContexts()->Add(new DungeonClearTriggerContext());
        dc_access::SharedValueContexts()->Add(new DungeonClearValueContext());

        LOG_INFO("module", "mod-dungeon-clear: registered DungeonClear contexts "
                           "(strategy/action/trigger/value) into all class "
                           "registries of mod-playerbots.");
    }

    // Stop + join the async pathfinding worker before maps unload (this fires
    // during the world shutdown sequence, ahead of OnAfterUnloadAllMaps). After
    // join, the worker's queue and mailbox are cleared, releasing every navmesh
    // shared_ptr it held — so no worker thread can touch a map being torn down.
    // Idempotent and safe even if the worker was never started.
    void OnShutdown() override
    {
        DcPathWorker::Instance().Stop();
    }

private:
    bool _registered = false;
};

class DungeonClearLoginPlayerScript : public PlayerScript
{
public:
    DungeonClearLoginPlayerScript() : PlayerScript("DungeonClearLoginPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        // Only bots (and self-bot players) have a PlayerbotAI. Give them the
        // single "dungeon clear" strategy on the non-combat engine. Harmless if
        // the contexts aren't registered yet — ChangeStrategy is a no-op for an
        // unknown name. Inert on a bot whose own "dungeon clear enabled" flag is
        // false (the tank flips it via `dc on`), so installing it on every bot
        // is safe: non-tank followers run only the follow-tank trigger, and only
        // while a party tank has DC enabled.
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
            return;

        if (!botAI->HasStrategy("dungeon clear", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("+dungeon clear", BOT_STATE_NON_COMBAT);
    }
};

// Drives the orphaned-follow reaper once per world tick. A non-tank DC follower
// installs a persistent MoveFollow generator to chase the tank; its own
// follow-tank action tears that down when the DC tank goes away — but only while
// the follower's PlayerbotAI is still ticking. A self-bot that leaves bot mode
// (`.playerbots bot self` off) has its PlayerbotAI deleted outright, so the
// teardown never runs and the now-human player stays glued to the tank. The
// reaper detects that orphaned generator and clears it, returning movement
// control to the player. OnPlayerbotUpdate is the lone playerbots-specific
// per-tick hook that fires regardless of whether the affected player still has
// an AI (it is a global tick, not a per-bot one), which is exactly what we need
// since the player we must fix no longer has a bot AI.
class DungeonClearReaperScript : public PlayerbotScript
{
public:
    DungeonClearReaperScript() : PlayerbotScript("DungeonClearReaperScript") {}

    void OnPlayerbotUpdate(uint32 /*diff*/) override
    {
        DungeonClearUtil::ReapOrphanedFollows();
        // Drop async path results that were never collected (the bot logged out
        // or toggled dc off before polling). Bounds the mailbox; cheap no-op
        // when empty.
        DcPathWorker::Instance().Sweep(DC_ASYNC_PATH_RESULT_TTL_MS);
    }
};

void AddSC_dungeon_clear_module()
{
    new DungeonClearRegistrarWorldScript();
    new DungeonClearLoginPlayerScript();
    new DungeonClearReaperScript();
}
