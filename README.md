# CS2-Bot-Locker

Metamod:Source native plugin. Two locks for CS2 bots:

- **Weapon lock** — pin a bot to one weapon slot; AI weapon switches are blocked.
- **Bot lock** — freeze the bot's `CCSBot::Update` tick entirely, so the AI stops driving the bot (used by demo replay to take over movement and aim).

**Version**: 0.2.0
**Native ABI**: 3

## Slot

| Target | Engine | Weapons |
|---|---|---|
| `Slot1` | 0 | Primary |
| `Slot2` | 1 | Pistol |
| `Slot3` | 2 | Knife / Zeus |
| `Slot4` | 3 | Grenades (HE/flash/smoke/molotov/decoy) |
| `Slot5` | 4 | C4 |

## Install

- `BotLocker.dll` -> `csgo/addons/BotLocker/bin/win64/`
- `gamedata.json` -> `csgo/addons/BotLocker/`
- `BotLocker.vdf` -> `csgo/addons/metamod/`

## Build

Env vars: `HL2SDKCS2`, `MMSOURCE_DEV`, `CSGO_PROTO`, `protoc` on PATH.

```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

## Console Commands

Weapon lock:
```
blw_lock <id> <slot1|slot2|slot3|slot4|slot5>
blw_unlock <id>
blw_unlock_all
blw_status
```

Bot lock:
```
bl_lock_bot <id>
bl_unlock_bot <id>
bl_unlock_all_bots
bl_status
```

## CounterStrikeSharp API

Drop `scripts/BotLocker.NativeApi.cs` into your project.

```csharp
using BotLockerApi;

BotLocker.IsCompatible();              // bool, check on load

// weapon lock
BotLocker.Lock(id, LockTarget.Slot3);  // bool
BotLocker.Unlock(id);                  // bool
BotLocker.UnlockAll();                 // void
BotLocker.GetLock(id);                 // LockTarget

// bot lock
BotLocker.LockBot(id);                 // bool
BotLocker.UnlockBot(id);               // bool
BotLocker.UnlockAllBots();             // void
BotLocker.IsBotLocked(id);             // bool
```

Main thread only.
