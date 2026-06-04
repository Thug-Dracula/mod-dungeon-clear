/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARUTIL_H
#define _PLAYERBOT_DUNGEONCLEARUTIL_H

#include <string>
#include <vector>

#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"

class Player;
class Unit;
class Creature;
class InstanceScript;
class PlayerbotAI;
class AiObjectContext;
struct DungeonBossInfo;

class DungeonClearUtil
{
public:
    // Returns the closest hostile, alive unit from `possibleTargets` that
    // sits within `range` of the bot AND within `halfAngle` radians of the
    // direction from bot to boss. Nullptr if none. Geometry-only — used as
    // the fallback when corridor computation isn't available.
    static Unit* FindBlockingTrash(Player* bot,
                                   DungeonBossInfo const& boss,
                                   float range,
                                   float halfAngle,
                                   GuidVector const& possibleTargets);

    // Computes the mmap path from the bot's current position to (bx, by, bz).
    // Returns true and fills `out` only when the result is PATHFIND_NORMAL —
    // a complete navigable route. Empty/incomplete/shortcut results return
    // false. The caller uses the polyline to test "is any hostile within
    // corridorWidth yards of the next maxLookAhead yards of the path."
    static bool ComputeCorridor(Player* bot,
                                float bx, float by, float bz,
                                Movement::PointsArray& out);

    // Returns the closest hostile, alive unit from `possibleTargets` whose
    // 2D distance to the path polyline is <= corridorWidth, considering
    // only the segment of the path within the first maxLookAhead yards
    // from the bot. LOS-checked. Nullptr if none.
    static Unit* FindBlockingTrashCorridor(Player* bot,
                                           Movement::PointsArray const& corridor,
                                           float maxLookAhead,
                                           float corridorWidth,
                                           GuidVector const& possibleTargets);

    // Returns a live spawned creature with the given entry on the bot's map, or
    // nullptr if none exists or all are dead.
    static Creature* FindLiveCreatureOnMap(Player* bot, uint32 entry);

    // Live boss creature for `entry`, resolved through the cached
    // "dungeon clear live boss" GUID (O(1) ObjectAccessor lookup) instead of
    // re-scanning the whole creature store on every call — the trigger ladder
    // and advance action ask for it several times per tick. Falls back to a
    // direct store scan when the cache was computed for a different boss (the
    // brief window just after a boss change) or the cached GUID went stale.
    // Returns nullptr when the boss isn't loaded/alive on the map.
    static Creature* GetLiveBoss(Player* bot, AiObjectContext* ctx, uint32 entry);

    // Returns true if at least one spawned creature with the given entry exists
    // on the bot's map (alive or dead). Distinguishes "missing" from "killed".
    static bool IsCreaturePresentOnMap(Player* bot, uint32 entry);

    // Returns true when the party has caught up and recovered enough to pull again:
    //  - every living party member on the bot's map has HP% >= minHpPct,
    //  - every living mana-using party member has mana% >= minMpPct,
    //  - every living member is within maxSpread yards of the bot.
    // Dead members are not blocking — the party-died trigger handles them.
    static bool IsPartyReady(Player* bot, float minHpPct, float minMpPct, float maxSpread);

    // Returns true if any LIVING bot party member on the bot's map (excluding
    // `bot` itself) currently has a corpse it intends to loot — in any phase,
    // walking in (has available loot) or within reach (can loot). Reads each
    // member's own loot values cross-context (same pattern as
    // DungeonClearPartyTankValue); real players (no PlayerbotAI) are skipped
    // since we neither drive nor wait on their looting. Lets the dungeon-clear
    // tank hold its advance after a pull until the whole party has finished
    // looting; the caller bounds the wait with a commit-timeout.
    static bool IsAnyPartyMemberLooting(Player* bot);

    // Builds a short, human-readable account of who the tank is waiting on to
    // become pull-ready, using the SAME thresholds IsPartyReady is called with
    // (so the description always matches the gate that actually holds the
    // advance). Lists each living on-map member that is too far, low on health,
    // or low on mana, with the limiting reason — e.g. "Bob (low HP), Alice (out
    // of range)". Caps the list so the addon line stays short; extra members
    // collapse to "+N more". Returns "" when the party is ready (nobody to wait
    // on). Used by DcStatusAction to fill the addon "resting" detail.
    static std::string DescribePartyNotReady(Player* bot,
                                             float minHpPct, float minMpPct,
                                             float maxSpread);

    // Names the living bot party members currently looting (walking to or
    // standing on a corpse), comma-joined and capped like DescribePartyNotReady.
    // Returns "" when only the tank itself is looting / nobody is. Used to fill
    // the addon "looting" detail so the player can see who is holding up the
    // advance.
    static std::string DescribePartyLooting(Player* bot);

    // --- Per-corpse loot give-up list ---------------------------------------
    // Prunes expired give-up entries and strips the still-live ones from the
    // stock "available loot" stack, additionally clearing "loot target" when it
    // points at a skipped GUID (can-loot reads the target, not the stack). Call
    // at the top of every DC loot-yield decision — because the advance /
    // follow-tank actions run at higher relevance than the loot pipeline, this
    // executes before stock picks its nearest target, so both the loot flags
    // AND stock's target selection skip loot the bot already gave up on. No-op
    // when the give-up list is empty (the happy path is untouched). Module-only:
    // it mutates the stock loot values, never stock code.
    static void StripSkippedLoot(PlayerbotAI* botAI);

    // Marks the loot the bot is currently committed to — the stock "loot
    // target" if set, else the nearest entry in the available-loot stack — as
    // given-up for `ttlMs`. Called when a loot yield times out so the bot stops
    // re-committing to a corpse/chest it can't finish. No-op when nothing of the
    // bot's own is resolvable (e.g. a tank whose yield is only IsAnyPartyMember-
    // Looting waiting on a follower — that follower gives up its own loot).
    static void GiveUpCurrentLoot(PlayerbotAI* botAI, uint32 ttlMs);

    // Fast-skips a corpse the bot has been "camped" on — standing within
    // interaction range (can-loot true) of one specific plain corpse — for
    // longer than campTimeoutMs. A normal loot transaction clears in a tick or
    // two once the bot is in range, so a long camp means the loot is
    // un-finishable for this bot (group-roll items pending a real player's roll,
    // items reserved for others, bags full). Rather than waiting out the much
    // longer loot-yield timeout on every such corpse — the "stuck on certain
    // corpses" stall — this blacklists it for giveUpTtlMs and strips it right
    // away so the loot flags drop this tick. Gathering nodes (skinning / mining
    // / herbalism) are exempt: their multi-second cast is legitimate camping.
    // Tracks the camped GUID + arrival time in the "dungeon clear loot camp *"
    // values; resets them whenever the bot isn't in range of a corpse. Returns
    // true when it skipped a corpse this call. Module-only: mutates stock loot
    // values via GiveUpCurrentLoot / StripSkippedLoot, never stock code.
    static bool MaybeGiveUpCampedLoot(PlayerbotAI* botAI, uint32 campTimeoutMs, uint32 giveUpTtlMs);

    // Decides, BEFORE the bot walks over, whether the corpse it is about to
    // commit to (stock "loot target", else the nearest entry in "available
    // loot") is worth a stop — and if not, blacklists + strips it this tick so
    // the bot never detours to it (or, if already parked, leaves at once)
    // instead of discovering its emptiness by camping out the loot-yield /
    // camp timeouts. This is the proactive, event-driven counterpart to
    // MaybeGiveUpCampedLoot: creature loot is generated at kill time, so the
    // contents are knowable without opening the corpse.
    //
    // A corpse is skipped when it holds nothing this bot can take, which is two
    // overlapping cases:
    //   - un-finishable: every unlooted item is locked in a group-loot roll the
    //     bot has not won (won items are auto-delivered, never re-looted),
    //     reserved for another looter, disallowed for the bot, or the bags are
    //     full — and there is no claimable gold.
    //   - below policy: no unlooted item meets DungeonClear.LootMinQuality (an
    //     ITEM_QUALITY_* floor). Quest / quest-starter items always qualify.
    //     A floor above 0 also stops gold alone from earning a detour, so the
    //     floor actually reduces the number of stops; floor 0 (default)
    //     preserves stock "loot everything" behaviour.
    //
    // Only plain creature corpses are judged. Gathering nodes (non-zero skillId),
    // chests, gameobjects and item loot are left to stock (returns false). The
    // check is deliberately conservative — it skips only when confident nothing
    // is takeable — because a false skip drops real loot, whereas a missed skip
    // merely falls back to the existing timeout. Returns true when it skipped a
    // corpse this call. Module-only: mutates stock loot values via
    // GiveUpCurrentLoot / StripSkippedLoot, never stock code.
    static bool MaybeSkipUnworthyLoot(PlayerbotAI* botAI, uint32 giveUpTtlMs);

    // Returns true if `creature`'s corpse holds at least one item this bot can
    // take right now (allowed, not blocked in / lost to a group roll, meeting
    // the minQuality ITEM_QUALITY_* floor — quest items always pass) or, when
    // minQuality is 0, claimable gold. Inspects the server-side creature loot,
    // filled at death. Used by MaybeSkipUnworthyLoot; returns true (don't skip)
    // for anything it cannot positively classify.
    static bool CorpseHasTakeableLoot(Player* bot, Creature* creature, uint32 minQuality);

    // Returns true if at least one navigable chunk of the path to (x, y, z)
    // exists. Delegates to ChunkedPathfinder::IsReachable, which is more
    // permissive than the legacy strict-PATHFIND_NORMAL check — partial
    // paths are acceptable because Advance now chunks long routes hop by hop.
    // The strict per-tick "actually making forward progress" check lives in
    // the position-based stuck detector inside DungeonClearAdvanceAction.
    static bool IsReachable(Player* bot, float x, float y, float z);

    // Vertical-aware reachability gate for trash/proximity target selection.
    // A candidate within DC_Z_LEVEL_TOLERANCE of the bot's Z is treated as
    // same-level and accepted with no pathfinder probe (the cheap path for
    // flat corridors). Beyond that tolerance the candidate is on a different
    // level — a balcony above, a pit below — where line-of-sight through
    // railings or floor gaps routinely reports a target the bot cannot path
    // to. For those we run a single short-range PathGenerator probe and accept
    // only a PATHFIND_NORMAL route whose endpoint actually lands on the
    // candidate's level. This stops the tank from locking onto an above/below
    // mob it can never reach and wedging against the geometry. Trash is always
    // within a corridor/cone lookahead well under PathGenerator's single-call
    // range cap, so one direct probe is both cheaper and more precise here than
    // the strided boss pathfinder behind IsReachable.
    static bool IsLevelReachable(Player* bot, Unit* u);

    // Scans `candidates` for the closest hostile alive unit whose 2D distance
    // to any segment of the supplied path polyline (within the first
    // `maxLookAhead` yards of forward travel) is ≤ corridorWidth. LOS-checked.
    // Nullptr if none. Replaces the single-segment FindBlockingTrashCorridor
    // for long routes that fan across multiple chunks.
    static Unit* FindBlockingTrashOnPath(Player* bot,
                                         std::vector<PathSegment> const& segments,
                                         float maxLookAhead,
                                         float corridorWidth,
                                         GuidVector const& candidates);

    // Scans every loaded creature on the bot's map for an alive hostile that
    // (a) is not already in combat with someone else and (b) the bot can path
    // to. Returns the closest such unit, or nullptr. Used by the stalled
    // fallback to kill obstacles when no path to the boss exists.
    static Unit* FindNearestReachableHostile(Player* bot);

    // Returns the InstanceScript driving the bot's current map, or nullptr
    // if the map is not an instance map or has no script. Used by the
    // next-boss probe to consult authoritative encounter state before
    // falling back to spawn-store creature scanning.
    static InstanceScript* GetInstanceScript(Player* bot);

    // Send a structured addon message with prefix "DC" to all real players in the bot's group.
    static void SendAddonMessage(PlayerbotAI* botAI, std::string const& msg);
};

#endif
