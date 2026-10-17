# CS2-Bot-Controller

## or CS2-Bot-Mimic2

**Make Bot Mimic Again**

> Developer guide: [TECH.md](TECH.md).

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`CS2-Bot-Controller` is a plugin for **Counter-Strike 2** that takes control of bot behavior at the engine level.

It can:

- lock a bot's weapon
- freeze a bot's aim
- fully freeze a bot for external control
- record a player's movement
- replay that movement on any bot
- inject concurrent, cancellable usercmd buttons with optional durations

------------------------------------------------------------------------

## Install

1. Install Metamod:Source and your chosen framework (CounterStrikeSharp or SwiftlyS2).
2. From [Releases](https://github.com/XBribo/CS2-Bot-Controller/releases/latest), download the Metamod package for your platform and the matching CSS or SW2 API package.
3. Copy both packages' `addons/` folders into `game/csgo/`.
4. Restart the server.

------------------------------------------------------------------------

## Console Commands

| Command | Description |
| --------- | ------------- |
| `bc_lock <all\|aim\|weapon> <slot> [slot1..slot5]` | Apply a lock to a bot slot |
| `bc_unlock <all\|aim\|weapon> <slot>` | Remove a lock from a bot slot |
| `bc_unlock_all <all\|aim\|weapon>` | Remove one lock kind from all bot slots |
| `bc_status` | Show current hook status and per-slot lock state |
| `bc_buy <slot> <alias> [alias...]` | Set a bot's buy plan for each round |
| `bc_buy_skip <slot>` | Make a bot skip buying each round |
| `bc_unbuy <slot>` | Remove a bot's buy plan |
| `bc_unbuy_all` | Remove all buy plans |
| `bc_profile <slot>` | Show a bot's profile and weapon preferences |

`weapon` mode requires the weapon slot as the third argument. Player slots are engine slots, not user IDs or entity indices.

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

`fileName` is optional. When omitted, the invoking player's SteamID is used. These commands require the CSS or SW2 provider.

- API usage, build details, ABI notes, and integration examples are documented in [TECH.md](TECH.md).

------------------------------------------------------------------------

## Special thanks

- [cs2kz-metamod](https://github.com/KZGlobalTeam/cs2kz-metamod) for helping determine the replay framework.
- [御坂17032号](https://github.com/Misaka17032) and [Miksen](https://github.com/mrc4tt) for adding Linux support to the plugin.
- [nicedayzhu](https://github.com/nicedayzhu) for adding swiftlys2 api support to the plugin.
- [un1](https://github.com/unicbm) for the voice framework and signature updates.

------------------------------------------------------------------------

## License

CS2-Bot-Controller is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See the LICENSE file for details.

------------------------------------------------------------------------

## Author

- **XBribo**
- Other contributors
