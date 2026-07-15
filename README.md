# CS2-Bot-Controller

## or CS2-Bot-Mimic2

**Make Bot Mimic Again**

> For developer, see [TECH.md](TECH.md).

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`CS2-Bot-Controller` is a plugin for **Counter-Strike 2** that takes control of bot behavior at the engine level.

It can:

- lock a bot's weapon
- freeze a bot's aim
- stop a bot from jumping
- fully freeze a bot for external control
- record a player's movement
- replay that movement on any bot

------------------------------------------------------------------------

## Install

1. Download the latest `BotController-MM-windows.zip` or `BotController-MM-linux.zip` from the [Releases page](https://github.com/XBribo/CS2-Bot-Controller/releases/latest).
2. Download the latest `BotController-CSS-API.zip (CounterStrikeSharp)`
or `BotController-SW2-API.zip (SwiftlyS2)` from the [Releases page](https://github.com/XBribo/CS2-Bot-Controller/releases/latest).
3. Extract the archive and copy the `/addons/` folder into your server's `/game/csgo/` directory.
4. Restart the server.

------------------------------------------------------------------------

## Console Commands

| Command | Description |
|---------|-------------|
| `bc_lock <all\|aim\|jump\|weapon> <slot> [slot1..slot5]` | Apply a lock to a bot slot |
| `bc_unlock <all\|aim\|jump\|weapon> <slot>` | Remove a lock from a bot slot |
| `bc_unlock_all <all\|aim\|jump\|weapon>` | Remove one lock kind from all bot slots |
| `bc_status` | Show current hook status and per-slot lock state |

`weapon` mode requires the weapon slot as the third argument.

Examples:

```text
bc_lock aim 1
bc_lock weapon 1 slot3
bc_unlock_all weapon
bc_status
```

------------------------------------------------------------------------

## Chat Commands

```text
!record [fileName]
!stoprecord
!replay <botSlot> [fileName]
!stopreplay <botSlot>
```

`fileName` is optional. When omitted, the invoking player's SteamID is used.

- API usage, build details, ABI notes, and integration examples are documented in [TECH.md](TECH.md).

------------------------------------------------------------------------

## Special thanks

- [cs2kz-metamod](https://github.com/KZGlobalTeam/cs2kz-metamod) for helping determine the replay framework.
- [Misaka17032](https://github.com/Misaka17032) for adding Linux support to the plugin.
- [nicedayzhu](https://github.com/nicedayzhu) for adding swiftlys2 api support to the plugin.
- [unicbm](https://github.com/unicbm) for the voice framework and signature updates.

------------------------------------------------------------------------

## License

CS2-Bot-Controller is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See the LICENSE file for details.

------------------------------------------------------------------------

## Author

- **XBribo**
- Other contributors
