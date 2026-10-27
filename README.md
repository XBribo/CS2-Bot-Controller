<div align="center">

# BotController

**Make Bot Mimic Again**

<p>
  <a href="https://github.com/XBribo/CS2-Bot-Controller/releases/latest"><img src="https://img.shields.io/github/v/release/XBribo/CS2-Bot-Controller?style=flat-square" alt="Release"></a>
  <a href="https://github.com/XBribo/CS2-Bot-Controller/releases/latest"><img src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-0078D4?style=flat-square" alt="Platforms"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="License"></a>
</p>

[Quick Start](#quick-start) · [Commands](#commands) · [Record and Replay](#record-and-replay) · [Developer Guide](TECH.md)

</div>

A Counter-Strike 2 plugin for bot control.

- Lock weapons, freeze aim, or pause bot AI for external control.
- Record player movement and replay it on bots.
- Inject concurrent, cancellable usercmd buttons with optional durations.

### Signature Status

[![CS2 Bot Signatures Tracker](https://raw.githubusercontent.com/XBribo/cs2-bot-signatures/main/.github/docs/summary.svg)](https://github.com/XBribo/cs2-bot-signatures)

## Quick Start

1. Install Metamod:Source and either CounterStrikeSharp (CSS) or SwiftlyS2 (SW2).
2. Download your platform's Metamod package and matching CSS or SW2 API package from [Releases](https://github.com/XBribo/CS2-Bot-Controller/releases/latest).
3. Copy both packages' `addons/` folders into `game/csgo/` and restart the server.
4. Run `bc_status` to check hooks and bot state.

## Commands

Use engine player slots (`0–63`).

| Command | Description |
| --- | --- |
| `bc_lock <all\|aim\|weapon> <slot> [slot1..slot5]` | Pause bot AI, lock aim, or lock weapon selection |
| `bc_unlock <all\|aim\|weapon> <slot>` | Remove a lock |
| `bc_unlock_all <all\|aim\|weapon>` | Remove one lock kind from all bots |
| `bc_status` | Show hooks, locks, and replay state |
| `bc_buy <slot> <alias> [alias...]` | Set a per-round buy plan |
| `bc_buy_skip <slot>` | Skip buying each round |
| `bc_unbuy <slot>` | Remove a buy plan |
| `bc_unbuy_all` | Remove all buy plans |
| `bc_profile <slot>` | Show profile and weapon preferences |

`weapon` requires a weapon slot (`slot1`–`slot5`) as the third argument:

```text
bc_lock aim 1
bc_lock weapon 1 slot3
bc_unlock_all weapon
bc_status
```

## Record and Replay

Chat commands require the CSS or SW2 provider:

```text
!record [fileName]
!stoprecord
!replay <botSlot> [fileName]
!stopreplay <botSlot>
```

Omit `fileName` to use your SteamID. Record and replay at the same tickrate.

## Credits

Created by [XBribo](https://github.com/XBribo), with thanks to [un1](https://github.com/unicbm), [御坂17032号](https://github.com/Misaka17032), [Miksen](https://github.com/mrc4tt), [nicedayzhu](https://github.com/nicedayzhu), and all contributors.

[![BotController contributors](https://contrib.rocks/image?repo=XBribo/CS2-Bot-Controller)](https://github.com/XBribo/CS2-Bot-Controller/graphs/contributors)

Star BotController to support development.

## License

CS2-Bot-Controller is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See [LICENSE](LICENSE) for details.
