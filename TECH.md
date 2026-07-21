# TECH.md

## Technical Overview

Current ABI: `16`

------------------------------------------------------------------------

## Build

Environment:

- `HL2SDKCS2`
- `MMSOURCE_DEV`
- `CSGO_PROTO`
- `protoc` 3.21.x on `PATH`

Native build:

```text
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Unified packaging script:

```powershell
.\build.ps1
```

Current `dist/` layout:

- `dist/windows/` → Windows native Metamod package only
- `dist/linux/` → Linux native Metamod package only
- `dist/CounterStrikeSharp/` → CounterStrikeSharp package only
- `dist/SwiftlyS2/` → SwiftlyS2 package only

------------------------------------------------------------------------

## Lock Model

- `Weapon` — pin a bot to one weapon slot; AI weapon switching is blocked
- `Aim` — freeze `CCSBot::Upkeep`; the bot can still move and decide
- `Jump` — block `CCSBot::Jump` only
- `All` — freeze both `CCSBot::Update` and `CCSBot::Upkeep`

------------------------------------------------------------------------

## Weapon Slots

| Target  | Engine | Weapon       |
| ------- | ------ | ------------ |
| `Slot1` | 0      | Primary      |
| `Slot2` | 1      | Pistol       |
| `Slot3` | 2      | Knife / Zeus |
| `Slot4` | 3      | Grenades     |
| `Slot5` | 4      | C4           |

------------------------------------------------------------------------

## Usercmd Injection

The managed API exposes token-based usercmd injection:

- `long InjectUsercmd(int slot, ulong buttonMask, int durationMs = 0)` creates an independent injection and returns a positive token, or `-1` on failure
- `bool CancelUsercmdInjection(int slot, long injectionId)` interrupts one injection.
`durationMs = 0` presses the buttons for one command and releases them on the next command.
A positive duration holds the buttons from the first injected command until the duration expires.

Injection fails when the slot, button mask, or duration is invalid, or when the native `PlayerRunCommand` hook is unavailable.

Button masks use the engine `IN_*` values. For example, CounterStrikeSharp exposes weapon inspect as `PlayerButtons.Inspect`:

```csharp
int botSlot = 1;
ulong inspectMask = (ulong)PlayerButtons.Inspect;
long inspectInjection = _bots?.InjectUsercmd(botSlot, inspectMask) ?? -1;

ulong attackMask = (ulong)PlayerButtons.Attack;
long attackInjection = _bots?.InjectUsercmd(botSlot, attackMask, 250) ?? -1;
if (attackInjection > 0)
    _bots?.CancelUsercmdInjection(botSlot, attackInjection);
```

------------------------------------------------------------------------

## Record And Replay

The native plugin records:

- origin
- velocity
- view angles
- button states
- duck state
- ladder state
- active weapon
- subtick input steps

Typical flow:

1. Lock the source slot if needed.
2. Call `StartRecord`.
3. Move the player.
4. Call `StopRecord`.
5. Move the recording to a bot with `TransferRecordingToReplay`, or load stored data with `LoadReplay`.
6. Apply `Lock(All)` to the bot slot.
7. Call `StartReplay`.

------------------------------------------------------------------------

## CounterStrikeSharp Integration

### Shared Capability

Reference the deployed shared contract:

```xml
<Reference Include="BotControllerApi">
  <HintPath>../addons/counterstrikesharp/shared/BotControllerApi/BotControllerApi.dll</HintPath>
  <Private>false</Private>
</Reference>
```

Runtime usage:

```csharp
using BotControllerApi;

private IBotControllerApi? _bots;

public override void OnAllPluginsLoaded(bool hotReload)
{
    _bots = BotControllerCapability.Cap.Get();
}
```

------------------------------------------------------------------------

## SwiftlyS2 Integration

### Shared Interface

The plugin exposes `IBotControllerApi` through `IInterfaceManager` using the key:

```text
botcontroller:api
```

Reference the exported contract DLL in a consumer plugin:

```xml
<Reference Include="BotControllerApi">
  <HintPath>../BotControllerImplSW2/resources/exports/BotControllerApi.dll</HintPath>
  <Private>false</Private>
</Reference>
```

Runtime usage:

```csharp
using BotControllerApi;
using SwiftlyS2.Shared;

public override void UseSharedInterface(IInterfaceManager interfaceManager)
{
    if (interfaceManager.TryGetSharedInterface<IBotControllerApi>(
            "botcontroller:api", out var api))
    {
        _bots = api;
    }
}
```

The plugin only registers this shared interface when the native BotController ABI
is available and compatible.

------------------------------------------------------------------------

## API Notes

Key operations exposed by both managed integrations:

- `Lock`
- `Unlock`
- `UnlockAll`
- `StartRecord`
- `StopRecord`
- `GetRecordedMotion`
- `TransferRecordingToReplay`
- `LoadReplay`
- `StartReplay`
- `StopReplay`
- `TryGetReplayTick`
- `SwitchBotWeapon`
- `InjectUsercmd`
- `CancelUsercmdInjection`
- `GetBotProfile`
- buy-plan operations

------------------------------------------------------------------------

## License

CS2-Bot-Controller is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See the LICENSE file for details.

------------------------------------------------------------------------

## Author

- **XBribo**
- Other contributors
