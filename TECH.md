# BotController Developer Guide

Installation and usage: [README.md](README.md).

- [Build](#build)
- [C# Integration](#c-integration)
- [Record and Replay](#record-and-replay)
- [API Reference](#api-reference)

## Build

Run commands from the repository root.

### Requirements

- **Windows:** Visual Studio 2026 C++ build tools and CMake.
- **Linux:** `g++`, CMake, and Make.
- **C#:** .NET 10 SDK.
- **Build script:** PowerShell 7+; WSL for Linux builds.
- **Dependencies:** Git and the `cs2_sdk` submodule.

```bash
git submodule update --init --recursive
```

### Build Script

On a Windows host:

```powershell
./build.ps1                     # Both platforms and providers
./build.ps1 -Windows            # Windows and both providers
./build.ps1 -Linux              # Linux (WSL) and both providers
./build.ps1 -Windows -SkipCsharp # Windows native plugin only
```

- Defaults: `Visual Studio 18 2026`, `Ubuntu-24.04`, `Release`.
- Overrides: `-Generator`, `-WslDistro`, `-Config`.
- `dist/` is regenerated; `-Clean` also clears build outputs.

| Directory | Contents |
| --- | --- |
| `dist/windows/` | Windows Metamod plugin |
| `dist/linux/` | Linux Metamod plugin |
| `dist/CounterStrikeSharp/` | CounterStrikeSharp provider and API contract |
| `dist/SwiftlyS2/` | SwiftlyS2 provider and API contract |

### Manual Build

Windows:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Linux:

```bash
cmake -S . -B out/linux -DCMAKE_BUILD_TYPE=Release
cmake --build out/linux -j$(nproc)
```

C# providers, including the API contract:

```bash
dotnet build csharp/BotControllerImpl/BotControllerImpl.csproj -c Release
dotnet build csharp/BotControllerImplSW2/BotControllerImplSW2.csproj -c Release
```

Native output: `<build-directory>/package/addons/`. Use `build.ps1` for complete packages.

## C# Integration

### Contract and Compatibility

- Deploy the native plugin, shared API, and provider together; Metamod requires KHook support.
- Shared API: `IBotControllerApi`, key `botcontroller:api`.
- Reference `BotControllerApi.dll`; preserve its struct layouts.
- Slots: engine player slots `0–63`.

Set `HintPath` and `Private=false`:

```xml
<ItemGroup>
  <Reference Include="BotControllerApi">
    <HintPath>libs/BotControllerApi.dll</HintPath>
    <Private>false</Private>
  </Reference>
</ItemGroup>
```

### CounterStrikeSharp

Contract: `addons/counterstrikesharp/shared/BotControllerApi/BotControllerApi.dll`.

In your `BasePlugin`:

```csharp
using System.Collections.Generic;
using BotControllerApi;
using CounterStrikeSharp.API.Core.Capabilities;

private static readonly PluginCapability<IBotControllerApi> Bots = new("botcontroller:api");
private IBotControllerApi? _bots;

// Resolve the API after providers load.
public override void OnAllPluginsLoaded(bool hotReload)
{
    try { _bots = Bots.Get(); }
    catch (KeyNotFoundException) { _bots = null; }
}
```

### SwiftlyS2

Contract: `addons/swiftlys2/plugins/BotControllerImplSW2/resources/exports/BotControllerApi.dll`.

In your SwiftlyS2 plugin:

```csharp
using BotControllerApi;
using SwiftlyS2.Shared;

private IBotControllerApi? _bots;

// Resolve the shared API.
public override void UseSharedInterface(IInterfaceManager interfaceManager)
{
    _bots = interfaceManager.TryGetSharedInterface<IBotControllerApi>(
        "botcontroller:api", out var api) ? api : null;
}
```

Requires a compatible native bridge. Null-check `_bots`; release it on provider unload.

## Record and Replay

Records one frame per `PlayerRunCommand` for bots and players.

### Workflow

1. Source: `SetReplayPawn` → `StartRecord`.
2. `StopRecord` before reading or transferring buffers.
3. Target: `SetReplayPawn` → load or transfer → `StartReplay`.
4. Stop with `StopReplay`; loop with `StartReplay(slot, loop: true)`.

`SetReplayPawn` takes the current Pawn address (`nint`). Check each result.

### In-Memory Transfer

Transfer a recording with InputHistory:

```csharp
// Stop, transfer, and replay.
private static bool ReplayRecording(IBotControllerApi api, int sourceSlot, int targetSlot, nint targetPawn)
{
    if (!api.StopRecord(sourceSlot) || !api.SetReplayPawn(targetSlot, targetPawn))
        return false;

    return api.TransferRecordingToReplay(sourceSlot, targetSlot)
        && api.StartReplay(targetSlot);
}
```

### Buffers and InputHistory

After stopping the source and registering the target:

```csharp
var (ticks, subs, commands) = api.GetRecordedMotionExtended(sourceSlot);

bool loaded = api.LoadReplay(targetSlot, ticks, subs, commands);
```

With InputHistory:

```csharp
byte[] history = api.GetRecordedInputHistory(sourceSlot);
bool loaded = api.LoadReplay(targetSlot, ticks, subs, commands, inputHistory: history);
```

- `inputHistory`: optional; `null` or empty skips history.
- Use opaque history from the same recording with matching native/provider builds.
- History scope: attack timing, history angles, and interpolation.
- `GetRecordedMotion` returns ticks/subticks; `GetRecordedMotionExtended` adds command frames. Load, then `StartReplay`.

Storage: Brotli JSON (`.json.br`) with available history. See [MotionRecording.cs](csharp/BotControllerImpl/MotionRecording.cs).

### Lifecycle and Threading

- Pawn changes and disconnects stop the session. Re-register and restart.
- Main thread: engine calls. Workers: stopped-buffer copies and `LoadReplay`.
- Finish buffer reads before restarting recording on that slot.
- Match tickrates; bundled providers use 64 tick.
- Replay suppresses bot AI; independent locks remain after stopping.
- Replay requires view control; losing it stops replay.

## API Reference

Contract: [IBotControllerApi.cs](csharp/BotControllerApi/IBotControllerApi.cs). Types: [Types.cs](csharp/BotControllerApi/Types.cs).

| Area | Members |
| --- | --- |
| Locks | `Lock`, `Unlock`, `UnlockAll`, `IsLocked`, `GetWeaponLock` |
| Recording | `StartRecord`, `StopRecord`, `RecordedTickCount`, `GetRecordedMotion`, `GetRecordedMotionExtended`, `GetRecordedInputHistory` |
| Replay data | `SetReplayPawn`, `LoadReplay`, `TransferRecordingToReplay` |
| Replay control | `StartReplay`, `StopReplay`, `ReplayCursor`, `ReplayTotal`, `IsReplaying`, `TryGetReplayTick` |
| Weapons | `SwitchBotWeapon`, `BotActiveWeaponDef` |
| Buttons | `InjectUsercmd`, `CancelUsercmdInjection` |
| Movement | `StartUsercmdMovement`, `UpdateUsercmdMovement`, `CancelUsercmdMovement` |
| Suppression | `SuppressUsercmd`, `StartUsercmdSuppression`, `CancelUsercmdSuppression` |
| Bot profile | `GetBotProfile` |
| Buy plans | `SetBuyPlan`, `SetBuySkip`, `ClearBuyPlan`, `ClearAllBuyPlans`, `BuyPlanItemCount` |
| Voice | `CanSendVoice`, `GetVoiceStatus`, `SendVoiceFrame` |

### Locks and Weapons

- `Aim`: blocks `Upkeep`. `All`: blocks `Update` and `Upkeep`. Physics continues.
- Query AI locks with `IsLocked`, weapon slots with `GetWeaponLock`.
- Weapon helpers use item definition IDs for bots and players. Switch weapons while replay is stopped.

| Target | Engine slot | Weapon |
| --- | --- | --- |
| `Slot1` | `0` | Primary |
| `Slot2` | `1` | Pistol |
| `Slot3` | `2` | Knife / Zeus |
| `Slot4` | `3` | Grenades |
| `Slot5` | `4` | C4 |

### Input Controls

- Buttons: engine `IN_*` masks, such as `PlayerButtons.Attack` in CounterStrikeSharp.
- `InjectUsercmd`: duration in milliseconds; positive cancellation token, `-1` on failure.
- Zero duration: press one command, release on the next. Timers start on the first injected command.
- Persistent movement/suppression use cancellation tokens. Movement is clamped to `[-1, 1]`.
- Requires the usercmd hook. Replay clears and rejects injected controls.

### Buy Plans and Voice

- `BuyPlanItemCount`: `-1` no plan, `0` skip/empty, positive item count.
- Voice: encoded Opus. Check `CanSendVoice` or `GetVoiceStatus`; status `0` means ready.
