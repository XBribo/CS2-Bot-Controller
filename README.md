# CS2-Bot-Controller

## or CS2-Bot-Mimic2

**Make Bot Mimic Again**

## Your stars⭐ are my motivation to keep updating

CS2-Bot-Controller is a Metamod:Source plugin for Counter-Strike 2 that takes
control of a bot's behaviour at the engine level. It can pin a bot's weapon,
freeze its aim, stop it jumping, or hand its movement over to external code —
and it can **record** a human player's per-tick movement and **replay** it back
through any bot.

It exposes both in-game console commands and a C-ABI surface for
CounterStrikeSharp, so a plugin can record, transfer, and replay motion with a
few P/Invoke calls. Win64 and Linux (linuxsteamrt64) are both supported.

------------------------------------------------------------------------

## Locks

- **Weapon** — pin a bot to one weapon slot; AI switches are blocked.
- **Aim** — freeze `CCSBot::Upkeep`; view holds still, AI keeps deciding/moving.
- **Jump** — block `CCSBot::Jump`; bot stops jumping, move/fire/aim unaffected.
- **All** — freeze both `CCSBot::Update` and `CCSBot::Upkeep`, so external code
  (such as motion replay) can drive the bot entirely.

------------------------------------------------------------------------

## Record & Replay

Capture a slot's movement tick by tick — origin, velocity, view angles, button
states, duck/ladder state, active weapon and all subtick input steps — then load
it onto another slot and play it back. Replay is driven through the engine's own
movement path, so it reproduces the original motion subtick-accurate.

Typical flow: lock the source slot if needed → `StartRecord` → move → `StopRecord`
→ `TransferRecordingToReplay` into a bot slot → `Lock(All)` the bot →
`StartReplay`. See the CounterStrikeSharp API section below.

------------------------------------------------------------------------

## Slots

| Target  | Engine | Weapon                  |
| ------- | ------ | ----------------------- |
| `Slot1` | 0      | Primary                 |
| `Slot2` | 1      | Pistol                  |
| `Slot3` | 2      | Knife / Zeus            |
| `Slot4` | 3      | Grenades                |
| `Slot5` | 4      | C4                      |

------------------------------------------------------------------------

## Install

The build stages a ready-to-copy `addons/` tree under `build/package/`.

- `BotController.dll` → `csgo/addons/BotController/bin/win64/`
  (`.so` → `linuxsteamrt64/` on Linux)
- `gamedata.json` → `csgo/addons/BotController/`
- `BotController.vdf`  → `csgo/addons/metamod/`

------------------------------------------------------------------------

## Build

Env: `HL2SDKCS2`, `MMSOURCE_DEV`, `CSGO_PROTO`, `protoc` (3.21.x) on PATH.

```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Config sources (vdf + gamedata) live under `configs/addons/`; the build copies
them into the package tree automatically.

------------------------------------------------------------------------

## Commands

```
bc_lock <all|aim|jump|weapon> <slot> [slot1..slot5]
bc_unlock <all|aim|jump|weapon> <slot>
bc_unlock_all <all|aim|jump|weapon>
bc_status
```

`weapon` mode requires the weapon slot as the third argument.

```
bc_lock aim 1                # freeze bot 1's view, AI still runs
bc_lock jump 1               # bot 1 can no longer jump
bc_lock all 1                # full freeze (use this before replay)
bc_lock weapon 1 slot3       # force bot 1 to knife
bc_unlock_all weapon         # clear every weapon lock
bc_status                    # print hook status + every per-slot lock
```

Record / replay is driven through the C-ABI below, not console commands.

------------------------------------------------------------------------

## CounterStrikeSharp API

There are two ways to consume the API. Pick one.

### Option A — shared capability (recommended)

Other plugins reference the shared `BotControllerApi.dll` and obtain the API at
runtime through a CounterStrikeSharp capability. No P/Invoke, no ABI constant in
your project — when the native layer bumps its ABI, your plugin keeps working as
long as the interface methods you call are unchanged.

The build deploys the contract DLL to
`addons/counterstrikesharp/shared/BotControllerApi/BotControllerApi.dll`. The
provider plugin `BotControllerImpl` registers the implementation on load.

In your plugin's `.csproj`, reference the shared DLL without copying it (both
sides must load the one shared assembly). Point `HintPath` at the deployed
contract DLL (adjust the path to your server layout):

```xml
<Reference Include="BotControllerApi">
  <HintPath>../addons/counterstrikesharp/shared/BotControllerApi/BotControllerApi.dll</HintPath>
  <Private>false</Private>
</Reference>
```

Then grab the API and call it:

```csharp
using BotControllerApi;

private IBotControllerApi? _bots;

public override void OnAllPluginsLoaded(bool hotReload)
{
    _bots = BotControllerCapability.Cap.Get();
    if (_bots is null)
        Server.PrintToConsole("[MyPlugin] BotController not loaded.");
}

// ... later ...
_bots?.Lock(slot, LockKind.All);
if (_bots is not null && _bots.TryGetProfile(slot, out var prof))
    Server.PrintToConsole($"skill={prof.Skill}");
```

### Option B — static P/Invoke wrapper (legacy)

Drop `scripts/BotController.NativeApi.cs` into your project. Self-contained but
ABI-coupled: a native ABI bump means re-copying the file.

```csharp
using BotControllerApi;

if (!BotController.IsCompatible()) return;   // requires the matching ABI
```

The static `BotController.*` calls below mirror the `IBotControllerApi` methods
one-to-one, so either option uses the same names.

### Locks

```csharp
BotController.Lock(slot, LockKind.Aim);
BotController.Lock(slot, LockKind.Jump);
BotController.Lock(slot, LockKind.All);
BotController.Lock(slot, LockTarget.Slot3);   // weapon lock
BotController.Unlock(slot, LockKind.Aim);
BotController.UnlockAll(LockKind.Weapon);
BotController.IsLocked(slot, LockKind.Aim);
BotController.GetWeaponLock(slot);            // -> LockTarget
```

### Record & Replay

```csharp
// Record a slot's motion
BotController.StartRecord(srcSlot);
// ... player moves ...
BotController.StopRecord(srcSlot);

// Replay it on a bot
BotController.TransferRecordingToReplay(srcSlot, botSlot);
BotController.Lock(botSlot, LockKind.All);    // hand the bot over
BotController.StartReplay(botSlot, loop: false);

// Or pull the buffers out, persist them, and load later
var (ticks, subs) = BotController.GetRecordedMotion(srcSlot);
BotController.LoadReplay(botSlot, ticks, subs);

// Drive weapon/fire from the tick being replayed
if (BotController.TryGetReplayTick(botSlot, out var tick))
    BotController.SwitchBotWeapon(botSlot, tick.WeaponDefIndex);

BotController.ReplayCursor(botSlot);          // current tick, <0 if idle
BotController.ReplayTotal(botSlot);           // loaded tick count
BotController.StopReplay(botSlot);
```

`ReplayTick` / `SubtickMove` mirror the C++ struct layout byte-for-byte, so the
buffers can be serialized and reloaded across rounds. Main thread only.

------------------------------------------------------------------------

## Special thanks

- [cs2kz-metamod](https://github.com/KZGlobalTeam/cs2kz-metamod) for helping determine the replay framework.
- [Misaka17032](https://github.com/Misaka17032) for adding Linux support to the plugin.

------------------------------------------------------------------------

## License

CS2-Bot-Controller is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.

------------------------------------------------------------------------

## Author

**XBribo**
