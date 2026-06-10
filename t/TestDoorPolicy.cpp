/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <initializer_list>

#include "DcDoorPolicy.h"

// Fixtures are REAL Lock.dbc rows (decoded from the 3.3.5 client data) for the
// doors that drove the door-handling overhaul:
//
//   lock  85 — Deadmines Factory/Foundry/Mast Room doors, SM doors: an empty
//              lock row, zero requirements. Anyone opens with a click.
//   lock  86 — Deadmines Heavy Doors: single Quick Open slot, skill 0.
//   lock 202 — Deadmines Iron Clad Door: picklock 1 / quick open / blasting 50.
//   lock 299 — SM Herod's Door, Strat Scarlet-side doors: Scarlet Key (7146)
//              or picklock 175 (+ bare-hands and blasting slots).
//   lock 879 — Strat King's Square / Gauntlet / Service gates: Key to the City
//              (12382) or picklock 300 (+ bare-hands and blasting slots).

namespace
{
    using DcDoorPolicy::LockSlot;

    struct LockFixture
    {
        LockSlot slots[DcDoorPolicy::LOCK_SLOT_COUNT];
    };

    LockFixture MakeLock(std::initializer_list<LockSlot> typed)
    {
        LockFixture f;
        std::size_t i = 0;
        for (LockSlot const& s : typed)
            f.slots[i++] = s;
        return f;
    }

    // type / index / requiredSkill per slot, matching the DBC column order.
    LockFixture const LOCK_85 = MakeLock({});
    LockFixture const LOCK_86 = MakeLock({{LOCK_KEY_SKILL, LOCKTYPE_QUICK_OPEN, 0}});
    LockFixture const LOCK_202 = MakeLock({{LOCK_KEY_SKILL, LOCKTYPE_PICKLOCK, 1},
                                           {LOCK_KEY_SKILL, LOCKTYPE_QUICK_OPEN, 0},
                                           {LOCK_KEY_SKILL, LOCKTYPE_BLASTING, 50}});
    LockFixture const LOCK_299 = MakeLock({{LOCK_KEY_ITEM, 7146, 0},
                                           {LOCK_KEY_SKILL, LOCKTYPE_PICKLOCK, 175},
                                           {LOCK_KEY_SKILL, LOCKTYPE_QUICK_OPEN, 0},
                                           {LOCK_KEY_SKILL, LOCKTYPE_QUICK_CLOSE, 0},
                                           {LOCK_KEY_SKILL, LOCKTYPE_BLASTING, 175}});
    LockFixture const LOCK_879 = MakeLock({{LOCK_KEY_ITEM, 12382, 0},
                                           {LOCK_KEY_SKILL, LOCKTYPE_PICKLOCK, 300},
                                           {LOCK_KEY_SKILL, LOCKTYPE_QUICK_OPEN, 0},
                                           {LOCK_KEY_SKILL, LOCKTYPE_QUICK_CLOSE, 0},
                                           {LOCK_KEY_SKILL, LOCKTYPE_BLASTING, 300}});

    bool CanOpen(LockFixture const& f, bool lockEnforced,
                 uint32 heldItem = 0, int32 lockpick = -1)
    {
        return DcDoorPolicy::CanOpenSlots(
            f.slots, DcDoorPolicy::LOCK_SLOT_COUNT, lockEnforced,
            [heldItem](uint32 entry) { return heldItem && entry == heldItem; },
            lockpick);
    }
}

// An empty lock row imposes no requirement: anyone opens it, and the
// GO_FLAG_LOCKED flag changes nothing because there is nothing to enforce.
// This is the Deadmines Factory/Foundry/Mast Room case the old gate refused.
TEST(DcDoorPolicyTest, EmptyLockOpensForAnyone)
{
    EXPECT_TRUE(CanOpen(LOCK_85, /*lockEnforced*/ false));
    EXPECT_TRUE(CanOpen(LOCK_85, /*lockEnforced*/ true));
}

// A bare-hands locktype (Quick Open) opens for anyone when the GO is not
// flagged locked — the Deadmines Heavy Door case.
TEST(DcDoorPolicyTest, QuickOpenOpensBareHandedWhenUnenforced)
{
    EXPECT_TRUE(CanOpen(LOCK_86, /*lockEnforced*/ false));
}

// The same bare-hands slot does NOT count on a GO_FLAG_LOCKED door: flagged
// gates demand the real key/skill slot.
TEST(DcDoorPolicyTest, QuickOpenSuppressedWhenLockEnforced)
{
    EXPECT_FALSE(CanOpen(LOCK_86, /*lockEnforced*/ true));
}

// Strat King's Square Gate (flagged locked): bare hands fail despite the
// Quick Open slot; the Key to the City or lockpicking 300 succeed.
TEST(DcDoorPolicyTest, StratGateNeedsKeyOrLockpicking)
{
    EXPECT_FALSE(CanOpen(LOCK_879, /*lockEnforced*/ true));
    EXPECT_TRUE(CanOpen(LOCK_879, /*lockEnforced*/ true, /*heldItem*/ 12382));
    EXPECT_TRUE(CanOpen(LOCK_879, /*lockEnforced*/ true, 0, /*lockpick*/ 300));
    EXPECT_FALSE(CanOpen(LOCK_879, /*lockEnforced*/ true, 0, /*lockpick*/ 299));
}

// Herod's Door: Scarlet Key or lockpicking 175. The wrong key does not open.
TEST(DcDoorPolicyTest, HerodsDoorKeyOrLockpicking)
{
    EXPECT_FALSE(CanOpen(LOCK_299, /*lockEnforced*/ true));
    EXPECT_TRUE(CanOpen(LOCK_299, /*lockEnforced*/ true, /*heldItem*/ 7146));
    EXPECT_FALSE(CanOpen(LOCK_299, /*lockEnforced*/ true, /*heldItem*/ 12382));
    EXPECT_TRUE(CanOpen(LOCK_299, /*lockEnforced*/ true, 0, /*lockpick*/ 175));
}

// Iron Clad Door (flagged locked): only a rogue's lockpicking gets through —
// the blasting slot (seaforium) is not modelled, the quick-open slot is
// suppressed by the flag, so everyone else parks and waits for the cannon.
TEST(DcDoorPolicyTest, IronCladDoorOnlyLockpicking)
{
    EXPECT_FALSE(CanOpen(LOCK_202, /*lockEnforced*/ true));
    EXPECT_TRUE(CanOpen(LOCK_202, /*lockEnforced*/ true, 0, /*lockpick*/ 1));
    EXPECT_FALSE(CanOpen(LOCK_202, /*lockEnforced*/ true, 0, /*lockpick*/ 0));
}

// A spell-keyed slot is an unsatisfiable requirement for bots, not a free
// pass through the "no requirement seen" fallthrough.
TEST(DcDoorPolicyTest, SpellSlotIsARequirement)
{
    LockFixture const spellLock = MakeLock({{LOCK_KEY_SPELL, 12345, 0}});
    EXPECT_FALSE(CanOpen(spellLock, /*lockEnforced*/ false));
}

// An item slot with index 0 still marks the lock as requiring something
// (mirrors Spell::CanOpenLock's reqKey bookkeeping).
TEST(DcDoorPolicyTest, ZeroItemIndexStillARequirement)
{
    LockFixture const oddLock = MakeLock({{LOCK_KEY_ITEM, 0, 0}});
    EXPECT_FALSE(CanOpen(oddLock, /*lockEnforced*/ false));
}
