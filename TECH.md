# TECH.md

## Technical Overview

`CS2-Bot-Controller` consists of:

- a native Metamod:Source plugin that hooks bot behavior
- a shared managed contract assembly `BotControllerApi`
- a CounterStrikeSharp plugin
- a SwiftlyS2 plugin

Current ABI : `12`

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

`ReplayTick` and `SubtickMove` mirror the native C++ layout byte-for-byte.

------------------------------------------------------------------------

## CounterStrikeSharp Integration

The CounterStrikeSharp provider plugin targets `net10.0`.

### Recommended: Shared Capability

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

### Legacy: Static P/Invoke

Drop `scripts/BotController.NativeApi.cs` into your project and check ABI compatibility:

```csharp
using BotControllerApi;

if (!BotController.IsCompatible()) return;
```

------------------------------------------------------------------------

## SwiftlyS2 Integration

The SwiftlyS2 provider plugin targets `net10.0` and is implemented in
`csharp/BotControllerImplSW2/BotControllerImplSW2Plugin.cs`.

It reuses the same native wrapper source as the CounterStrikeSharp provider via a
linked compile item, so the P/Invoke surface is maintained in one place:

- `csharp/BotControllerImpl/BotController.NativeApi.cs`

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
