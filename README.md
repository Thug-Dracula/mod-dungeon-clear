# mod-dungeon-clear

Autonomous dungeon-clearing mode for **mod-playerbots** tank bots, packaged as a
drop-in AzerothCore module. A tank bot drives the party from boss to boss —
clearing trash, pathing the layout, pausing for loot, and handling doors and
stalls along the way. You deal damage and let the tank run the dungeon.

Routes are generated on the fly from the live navigation mesh — no waypoints or
hardcoded paths — so it works in any instance, raids included. It runs against a
**stock, unmodified mod-playerbots** checkout (no playerbots source edits).

> ## ❗ Use the companion addon
>
> [**mod-dungeon-clear-addon**](https://github.com/jrad7/mod-dungeon-clear-addon)
> is the recommended way to drive a clear: a movable in-game panel with On / Off
> / Skip / Pause buttons, a live status readout, and a per-boss list. The `dc`
> chat keywords and `.dc` command still work, but the addon is far easier.

## Requirements

- **mod-playerbots** installed and enabled. This module is an *extension* of the
  playerbots AI engine — it subclasses playerbots' strategy/action/trigger/value
  classes and links against them; it is not standalone.

## Install

1. Clone into `modules/mod-dungeon-clear/`.
2. Re-run CMake and rebuild the worldserver (`-DMODULES=static`).
3. Optionally copy `conf/mod_dungeon_clear.conf.dist` →
   `mod_dungeon_clear.conf`.
4. To enable chat keywords and follower behaviour for self-bots and
   mid-session bots, add to your deployed `playerbots.conf`:

   ```
   AiPlayerbot.NonCombatStrategies          = "+dungeon clear"
   AiPlayerbot.RandomBotNonCombatStrategies = "+dungeon clear"
   ```

## Usage

Both input methods control the same behaviour and act on the group's **tank**
bot. Commands must come from a real player in the bot's group; `.dc on` requires
being inside a dungeon.

| Slash command | In-party chat keyword |
|---|---|
| `.dc on` | `dc on` / `dungeon clear on` |
| `.dc off` | `dc off` / `dungeon clear off` |
| `.dc skip` | `dc skip` |
| `.dc pause` | `dc pause` / `dungeon clear pause` |
| `.dc pull` | `dc pull` / `dungeon clear pull` |
| `.dc status` | `dc status` |
| `.dc bosses` | `dc bosses` |

Non-tank party bots follow the tank only while it has dungeon clear enabled,
then revert to the player automatically.

## Pull modes

How the tank takes trash packs on the way to each boss. Choose from the addon's
pull control or the `dc pull` toggle.

| Mode | Behaviour | Speed | Risk |
|---|---|---|---|
| **Dynamic** *(recommended)* | Decide per pack: Leeroy a lone pack, pull a clustered or oversized one. | Middle | Middle |
| **Leeroy** | Walk straight into each pack and fight in place. | Fastest | Highest |
| **Advanced** | Pull every pack back to a held camp before fighting. | Slowest | Lowest |

**Dynamic is the recommended mode for most content** — it Leeroys the easy packs
at full speed and only pays the careful camp-pull cost on the dangerous ones,
estimating how many mobs would actually aggro and comparing that to a tunable
ceiling (`DungeonClear.PullDynamicMaxLeeroyMobs`). Use **Leeroy** in easy content
you can out-gear, or **Advanced** in hard content and raids where every pull
matters.

See the [Pull modes](https://github.com/jrad7/mod-dungeon-clear/wiki/Pull-Modes)
wiki page for how each mode works in detail and how to tune Dynamic.

## You can't play *as* the tank with dungeon clear on

Dungeon clear drives the **tank bot's** AI, so the tank must be a bot — if you
personally control the tank, the AI and you fight over the same character. The
exception is **self-bot mode**: turn your own character into a self-bot
(`.playerbots bot self`) and let the AI drive. If you want hands on the keyboard,
play a follower instead.

## Configuration

All `DungeonClear.*` options live in `conf/mod_dungeon_clear.conf.dist`, and most
are overridable live, per run, from the companion addon's Settings panel. Common
ones: loot quality floor, rest health/mana targets, and the Dynamic pull tuning.
See the
[Configuration reference](https://github.com/jrad7/mod-dungeon-clear/wiki/Configuration)
wiki page.

A few mod-playerbots ranges also affect runs (`LootDistance`, `ReactDistance`,
`SightDistance`, `FollowDistance`); the rest gate tracks playerbots' own
eat/drink thresholds automatically, so no playerbots config change is required.

## How it integrates

mod-playerbots exposes no extension API, so this module appends its context
factories into the engine's shared registries on the first world tick and
registers a `.dc` command plus a login hook for the `dungeon clear` strategy. It
touches **no** playerbots file. Details:
[How it integrates](https://github.com/jrad7/mod-dungeon-clear/wiki/How-It-Integrates).

## License

AGPL-3.0-or-later (inherited from mod-playerbots). See `LICENSE`.
