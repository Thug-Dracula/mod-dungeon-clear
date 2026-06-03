# mod-dungeon-clear

Autonomous dungeon-clearing mode for **mod-playerbots** tank bots, packaged as a
drop-in AzerothCore module. A tank bot drives the party from boss to boss,
clearing blocking trash, pathing around the dungeon, pausing for loot, and
handling doors/stalls along the way.

## Requirements

- **mod-playerbots** must be installed and enabled. This is an *extension* of the
  playerbots AI engine, not a standalone module; it subclasses playerbots'
  strategy/action/trigger/value classes and links against them.
- Works against a **stock, unmodified mod-playerbots** checkout: no playerbots
  source edits are required. See [How it integrates](#how-it-integrates).

## Install

1. Clone into `modules/`:
   ```
   modules/mod-dungeon-clear/
   ```
2. Re-run CMake and rebuild the worldserver (`-DMODULES=static`).
3. Copy `conf/mod_dungeon_clear.conf.dist` → `mod_dungeon_clear.conf` in your
   config dir (optional; only affects the DungeonClear log channel).

## Usage

Both input methods control the same behaviour. Drive the **tank** bot:

| Slash command (any time, no config) | In-party chat keyword |
|---|---|
| `.dc on` | `dc on` / `dungeon clear on` |
| `.dc off` | `dc off` / `dungeon clear off` |
| `.dc skip` | `dc skip` |
| `.dc status` | `dc status` |
| `.dc bosses` | `dc bosses` |

- The command must be issued by a real player in the bot's group; `.dc on`
  requires being inside a dungeon and acts on the group's tank bot.
- **Chat keywords + follow-tank need the `dungeon clear` strategy applied.** The
  module's login hook applies it automatically to bots present at login, but the
  reliable universal path, and the **only** one that reaches a self-bot created
  mid-session via `.playerbots bot self`, is the playerbots config. Add this to
  your deployed `playerbots.conf` (both fields, so player-bots, random bots and
  self-bots are all covered):
  ```
  AiPlayerbot.NonCombatStrategies       = "+dungeon clear"
  AiPlayerbot.RandomBotNonCombatStrategies = "+dungeon clear"
  ```
  The `.dc` slash command always works regardless. Non-tank party bots follow
  the tank only while the tank has dungeon clear enabled, then revert to the
  player automatically.

### You can't play *as* the tank with dungeon clear on

Dungeon clear drives the **tank bot's** AI; it steers movement, pulls, and
loots on the tank's behalf. The party's tank therefore has to be a bot, not
your own character: if you're personally controlling the tank, the AI and you
fight over the same character and movement stutters/fails.

The one exception is **self-bot mode**. If you turn your own character into a
self-bot (`.playerbots bot self`) and let the bot AI drive, your character *is*
the tank bot and dungeon clear works on it normally. Self-bots are built from
the playerbots config, so reaching them requires the `NonCombatStrategies`
config above (the login hook can't catch a bot created mid-session). For a
human who keeps hands on the keyboard, roll the tank as a normal player bot and
play one of the followers instead.

## Companion addon

`mod-dungeon-clear-addon` is a client-side WoW addon (interface 30300, patch
3.3.5a) that gives the feature a movable in-game panel instead of typing
`dc`/`.dc` commands. It is **optional**: everything it does is also reachable
via chat keywords and the `.dc` command, but it's the recommended way to drive
and monitor a clear.

### Install

Copy the `DungeonClear` addon folder (`DungeonClear.toc` + `DungeonClear.lua`)
into your client's `Interface/AddOns/` directory, then enable **DungeonClear**
on the character-select AddOns list. No server-side install is needed beyond the
module itself.

### Use

- `/dc` toggles the window open/closed. `/dc <sub> [param]` also forwards any
  raw subcommand (e.g. `/dc on`, `/dc skip`) to the tank bot.
- **Action row:** On / Off / Skip / Pause·Resume buttons. Pause holds the tank
  in place without ending the clear; the button relabels to Resume while paused.
- **Status panel:** live mode status (OFF / ON / PAUSED), current state
  (Advancing, Clearing Trash, Boss Fight, Looting, Resting, Door Blocked, Route
  Blocked, …), the next boss target, and a warning line when the tank stalls.
- **Boss list:** every boss in the dungeon with its status (Alive / Dead /
  Skipped / Missing) and a per-boss **Go** button that targets the tank at that
  boss (auto-enabling dungeon clear first if it's off). On a split/multi-wing map
  the list is filtered to the bot's nearest wing.
- **Tiny mode:** the **Tiny** button collapses the panel to a single-line
  readout (status dot + state + target boss). Left-click the dot to start the
  clear or toggle pause; right-click anywhere on the bar restores the full
  window. Window position, tiny/folded state, and visibility persist via
  `DungeonClearDB` saved variables.

### How it talks to the server

The addon and module communicate over a silent `DC`-prefixed addon message
channel on the `PARTY` distribution (no visible chat). The addon sends
`CMD\t<sub>[\t<param>]` payloads; the server's `DungeonClearAddonHook` intercepts
them (before normal chat processing) and dispatches to the group's tank bot,
exactly like the `.dc` command. The module pushes `STATUS`, `BOSS_START` /
`BOSS` / `BOSS_END`, `CHAT`, and `ERROR` payloads back so the panel renders live
state. Bot announcements arrive as `CHAT` lines (prefixed `[DC]`) instead of
party-chat spam. The addon polls `status` every ~2s while a clear runs and
re-requests the boss list on a 2s/5s cadence so the panel self-heals through
loading screens and bot-not-yet-in-instance gaps.

## How it integrates

mod-playerbots exposes no extension API, so this module:

1. Appends its four `DungeonClear*Context` factories into the engine's shared
   context registries on the first world tick (see `src/AiObjectContextAccess.h`
   and `src/DungeonClearModule.cpp`).
2. Registers a `.dc` command and a login hook for the `dungeon clear` strategy.

It touches **no** playerbots file. The trade-off: it couples to playerbots'
internal class shape, so an upstream rename of those registries would surface as
a **compile error** here (never a silent runtime failure).

## License

AGPL-3.0-or-later (inherited from mod-playerbots). See `LICENSE`.
