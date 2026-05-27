# mod-dungeon-clear

Autonomous dungeon-clearing mode for **mod-playerbots** tank bots, packaged as a
drop-in AzerothCore module. A tank bot drives the party from boss to boss,
clearing blocking trash, pathing around the dungeon, pausing for loot, and
handling doors/stalls along the way.

## Requirements

- **mod-playerbots** must be installed and enabled. This is an *extension* of the
  playerbots AI engine, not a standalone module — it subclasses playerbots'
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

Both input methods control the same behaviour — drive the **tank** bot:

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
  reliable universal path — and the **only** one that reaches a self-bot created
  mid-session via `.playerbots bot self` — is the playerbots config. Add this to
  your deployed `playerbots.conf` (both fields, so player-bots, random bots and
  self-bots are all covered):
  ```
  AiPlayerbot.NonCombatStrategies       = "+dungeon clear"
  AiPlayerbot.RandomBotNonCombatStrategies = "+dungeon clear"
  ```
  The `.dc` slash command always works regardless. Non-tank party bots follow
  the tank only while the tank has dungeon clear enabled, then revert to the
  player automatically.

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
