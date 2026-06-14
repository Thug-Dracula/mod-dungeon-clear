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
#include "AllCreatureScript.h"
#include "Creature.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"

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

#include "Util/DcSpectator.h"
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

// Spectator AI-mover window, opening half. Playerbots drives every bot's AI
// (including a self-bot's) from its PlayerScript::OnPlayerAfterUpdate, and many
// bot actions are injected into core packet handlers (HandleUseItemOpcode,
// HandleGameObjectUseOpcode, …) that silently drop when the session's m_mover
// isn't the player — exactly the state spectate creates (the mover is the
// camera dummy). Net effect: a spectating warlock self-bot could not soulstone
// itself, use a healthstone, potion, or eat/drink. Fix: bracket playerbots'
// hook with a scoped swap of m_mover back to the player.
//
// ORDERING CONTRACT (what makes the bracket work): hooks run in script
// registration order (ScriptRegistry::EnabledHooks appends in AddScript
// order), and the generated module loader registers mod-dungeon-clear before
// mod-playerbots (alphabetical) — so this Begin script, registered in
// AddSC_dungeon_clear_module(), fires BEFORE playerbots' UpdateAI. The End
// script is registered on the first world tick from
// DungeonClearRegistrarWorldScript, which lands it after every load-time
// script, closing the window AFTER playerbots. All three hooks run
// back-to-back on the player's map thread; client movement packets for the
// dummy are processed in the session phases, never inside the window.
class DungeonClearSpectatorMoverBeginScript : public PlayerScript
{
public:
    DungeonClearSpectatorMoverBeginScript()
        : PlayerScript("DungeonClearSpectatorMoverBeginScript", {
            PLAYERHOOK_ON_AFTER_UPDATE
        }) {}

    void OnPlayerAfterUpdate(Player* player, uint32 /*p_time*/) override
    {
        DcSpectator::BeginBotAiMoverWindow(player);
    }
};

// Closing half of the mover window — see DungeonClearSpectatorMoverBeginScript
// for the full story. NOT registered in AddSC_dungeon_clear_module(): it must
// be instantiated on the first world tick so it sorts after playerbots' hook.
class DungeonClearSpectatorMoverEndScript : public PlayerScript
{
public:
    DungeonClearSpectatorMoverEndScript()
        : PlayerScript("DungeonClearSpectatorMoverEndScript", {
            PLAYERHOOK_ON_AFTER_UPDATE
        }) {}

    void OnPlayerAfterUpdate(Player* player, uint32 /*p_time*/) override
    {
        DcSpectator::EndBotAiMoverWindow(player);
    }
};

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

        // Closing half of the spectator AI-mover window. Registered HERE (first
        // world tick, not AddSC) so its hook sorts after playerbots' UpdateAI
        // hook — see DungeonClearSpectatorMoverEndScript. Safe to register now:
        // map threads only run inside sMapMgr->Update, never concurrently with
        // this world-thread hook.
        new DungeonClearSpectatorMoverEndScript();

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

        // Combat-engine companion: the advanced-pull maneuver (leader, mid-pull),
        // the camp hold/assist for held followers, and the in-combat regroup that
        // keeps followers grouped on the tank. Resident on every bot so the leader
        // can run the pull-to-camp once it aggros and followers can regroup mid-
        // fight (the non-combat strategy can't — the combat engine takes over the
        // instant combat starts). Every trigger is inert unless a DC run is active.
        if (!botAI->HasStrategy("dungeon clear combat", BOT_STATE_COMBAT))
            botAI->ChangeStrategy("+dungeon clear combat", BOT_STATE_COMBAT);
    }
};

// Spectator-camera teardown safety net. A leaked possession leaves the human
// controlling nothing (their mover is a despawning/orphaned dummy), and only we
// can clean it up — so every exit path below calls DcSpectator::Stop, which is
// idempotent and free to over-call. Note `dc off`/pause deliberately do NOT end
// spectate: the feature is independent of the DC run (useful for watching any
// bot activity, or nothing at all) — don't "fix" that by adding teardown there.
class DungeonClearSpectatorPlayerScript : public PlayerScript
{
public:
    DungeonClearSpectatorPlayerScript()
        : PlayerScript("DungeonClearSpectatorPlayerScript", {
            PLAYERHOOK_ON_BEFORE_TELEPORT,
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_BEFORE_LOGOUT
        }) {}

    // Covers hearth, instance exit, summons, `.tele`, AND near-teleports —
    // a near-teleport during spectate would hang un-acked in playerbots
    // (its ack path does bot->m_mover->ToPlayer(), null while the mover is
    // the camera dummy). Never vetoes the teleport.
    bool OnPlayerBeforeTeleport(Player* player, uint32 /*mapid*/, float /*x*/,
                                float /*y*/, float /*z*/, float /*orientation*/,
                                uint32 /*options*/, Unit* /*target*/) override
    {
        DcSpectator::Stop(player);
        return true;
    }

    // Body death while the camera is away: release control back so the normal
    // ghost/release flow works. Core may also break the charm itself — Stop is
    // idempotent either way.
    void OnPlayerJustDied(Player* player) override
    {
        DcSpectator::Stop(player);
    }

    // Despawn the dummy before the session goes away; never let the
    // TempSummon outlive its possessor.
    void OnPlayerBeforeLogout(Player* player) override
    {
        DcSpectator::Stop(player);
    }
};

// Spectator-camera death guard. THE crash fix for "body dies while spectating":
// spectate possesses the camera dummy via a direct SetCharmedBy(player,
// CHARM_TYPE_POSSESS) — an *aura-less* possession. When the player body (the
// charmer) dies, Unit::setDeathState(JustDied) -> Unit::RemoveAllControlled ->
// Player::StopCastingCharm runs FIRST, deep inside Unit::Kill and long before
// Player::KillPlayer fires OnPlayerJustDied. StopCastingCharm only knows how to
// undo possession by removing the possess AURA (Player.cpp RemoveAurasByType
// SPELL_AURA_MOD_POSSESS); with no such aura, GetCharmGUID() stays set and the
// function hits its ABORT() — a hard server crash. The existing OnPlayerJustDied
// -> Stop net cannot prevent it; it runs too late.
//
// Fix: catch the lethal blow at the OnDamage chokepoint (Unit::DealDamage, fired
// with the final post-absorb damage and the victim's health NOT yet reduced —
// well before the `health <= damage` Kill at Unit.cpp). If a spectating player's
// body is about to die, tear the possession down cleanly here. DcSpectator::Stop
// runs the proper RemoveCharmedBy, so by the time setDeathState reaches
// StopCastingCharm there is no charm left to choke on, and the body dies into the
// normal ghost/release flow. Stop is idempotent; the OnPlayerJustDied/teardown
// nets stay as backstops for the rare non-DealDamage instakill paths.
class DungeonClearSpectatorDeathGuardScript : public UnitScript
{
public:
    DungeonClearSpectatorDeathGuardScript()
        : UnitScript("DungeonClearSpectatorDeathGuardScript", true, {
            UNITHOOK_ON_DAMAGE
        }) {}

    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!victim || !victim->IsPlayer())
            return;
        // Only a lethal blow matters — a survivable hit must not eject the human
        // from spectate. health is still the pre-damage value at this point.
        if (damage < victim->GetHealth())
            return;

        Player* player = victim->ToPlayer();
        if (DcSpectator::IsActive(player))
            DcSpectator::Stop(player);
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

    void OnPlayerbotUpdate(uint32 diff) override
    {
        DcFollowerLifecycle::ReapOrphanedFollows();
        // Authoritative advanced-pull passive teardown: release any follower DC
        // put passive once its leader is no longer mid-pull (released / dc off /
        // paused / death / gone), and trip the camp-safety valve for a held,
        // passive follower taking damage. Runs on the global tick so it fires even
        // for a follower whose own engines are passive-locked in combat.
        DcFollowerLifecycle::ReapStrandedPassives();
        // Drop async path results that were never collected (the bot logged out
        // or toggled dc off before polling). Bounds the mailbox; cheap no-op
        // when empty.
        DcPathWorker::Instance().Sweep(DC_ASYNC_PATH_RESULT_TTL_MS);
        // Event-driven status: recompute each clearing tank's status and emit a
        // STATUS/BOSS packet only on change (replaces the addon's old poll).
        // Internally throttled; a cheap no-op when no tank is clearing. Runs on
        // the global tick (not a per-bot one) so it keeps firing through boss
        // fights, when the bot's non-combat strategy engine is dormant.
        DcStatusPublisher::TickStatusPushes(diff);
    }
};

// WORKAROUND for an upstream Zul'Farrak core-script typo (fixed on branch
// fix/zf-sezziz-summon-coords): Shadowpriest Sezz'ziz's summon table lists
// several add coords as x=895 instead of 1895, so during his fight he summons
// Sandfury Acolytes/Zealots ~1000yd OUTSIDE the temple. There they are
// unreachable and never die, holding the whole party in combat indefinitely —
// which freezes the DungeonClear temple event, whose end gossips (open the door,
// then fight Bly) run on the NON-combat engine and so never get a tick. We can't
// fix that in SQL (the coords are hardcoded in C++), so until the core fix is
// deployed this hook despawns the stray adds the instant they spawn, letting
// combat end and the event finish.
//
// Surgical: only Sandfury Acolyte/Zealot SUMMONS on the Zul'Farrak map that
// appear far WEST of the temple (x < 1500; the temple sits at x~1873-1902, so no
// legitimate creature is there). The pyramid-wave summons of the same entries
// spawn at the correct x (>1873) and are untouched. Despawn is deferred a beat so
// it can't dangle the pointer the summon caller uses right after SummonCreature
// (Sezz'ziz calls AttackStart on the fresh add); 1000yd away, the few-hundred-ms
// of phantom threat before despawn is harmless.
class DungeonClearZfStraySummonScript : public AllCreatureScript
{
public:
    DungeonClearZfStraySummonScript() : AllCreatureScript("DungeonClearZfStraySummonScript") {}

    void OnCreatureAddWorld(Creature* creature) override
    {
        if (!creature || creature->GetMapId() != 209 /*Zul'Farrak*/)
            return;
        uint32 const entry = creature->GetEntry();
        if (entry != 8876 /*Sandfury Acolyte*/ && entry != 8877 /*Sandfury Zealot*/)
            return;
        if (!creature->IsSummon() || creature->GetPositionX() >= 1500.0f)
            return;  // a real wave spawn / static trash at the temple — leave it

        LOG_INFO("playerbots.dungeonclear",
                 "[DC] despawning stray Sezz'ziz summon {} (entry {}) at x={:.1f} "
                 "(ZF coord-typo workaround)",
                 creature->GetGUID().ToString(), entry, creature->GetPositionX());
        creature->DespawnOrUnsummon(Milliseconds(200));
    }
};

void AddSC_dungeon_clear_module()
{
    new DungeonClearRegistrarWorldScript();
    new DungeonClearLoginPlayerScript();
    // Opening half only — the End script registers on the first world tick so
    // it sorts after playerbots' AI-update hook (see the ordering contract).
    new DungeonClearSpectatorMoverBeginScript();
    new DungeonClearSpectatorPlayerScript();
    new DungeonClearSpectatorDeathGuardScript();
    new DungeonClearReaperScript();
    new DungeonClearZfStraySummonScript();
}
