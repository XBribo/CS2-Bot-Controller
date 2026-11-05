# BotController Developer Guide

CounterStrikeSharp and SwiftlyS2 plugins share the `botcontroller:api` interface. See the [README](README.md) for installation and server commands.

## Plugin integration

Reference the release's `BotControllerApi.dll`, not the provider. Use the contract, native plugin and provider from the same release. Do not bundle a second copy of the DLL.

```xml
<Reference Include="BotControllerApi">
  <HintPath>libs/BotControllerApi.dll</HintPath>
  <Private>false</Private>
</Reference>
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

public override void UseSharedInterface(IInterfaceManager interfaceManager)
{
    _bots = interfaceManager.TryGetSharedInterface<IBotControllerApi>(
        "botcontroller:api", out var api) ? api : null;
}
```

Check `_bots` before use and release it on provider unload.

## API reference

[`IBotControllerApi`](csharp/BotControllerApi/IBotControllerApi.cs) defines the methods; [Types.cs](csharp/BotControllerApi/Types.cs) defines the shared types. Slots are player slots (0–63), not user IDs.

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

- **Locks:** `Aim` freezes aim; `All` pauses bot AI, not physics. For a weapon lock, pass a `LockTarget` (`Slot1`–`Slot5`) and query it with `GetWeaponLock`.
- **Input:** Button masks use engine `IN_*` values. `InjectUsercmd` returns a positive token or `-1` on failure. A zero duration presses for one command. Persistent movement and suppression use cancellation tokens; movement is clamped to `[-1, 1]`. Replay clears injected controls and rejects new ones.
- **Buy plans:** `BuyPlanItemCount` returns `-1` for no plan, `0` to skip buying, or the number of items.
- **Voice:** Send encoded Opus frames only when `GetVoiceStatus()` returns `0`.

### Record and replay

Register the source's current pawn with `SetReplayPawn` before `StartRecord`. To play the recording on another bot:

```csharp
bool StartBotReplay(IBotControllerApi api, int source, int target, nint targetPawn)
{
    return api.StopRecord(source)
        && api.SetReplayPawn(target, targetPawn)
        && api.TransferRecordingToReplay(source, target)
        && api.StartReplay(target);
}
```

For file storage, copy ticks, subticks and commands with `GetRecordedMotionExtended`. Pass those buffers to `LoadReplay` before `StartReplay`. Optional input history comes from `GetRecordedInputHistory`; keep it with its original recording and matching provider build. The bundled provider uses [Brotli JSON](csharp/BotControllerImpl/MotionRecording.cs).

Call engine methods on the game thread. Stopped-buffer copies and `LoadReplay` can run on a worker. Finish copying before restarting a recording. A pawn change or disconnect ends the session; register the new pawn before restarting. Match recording and replay tickrates (64 for the bundled providers). Stopping replay does not clear independent locks.

## Build from source

Initialize submodules with `git submodule update --init --recursive`. On Windows, use `./build.ps1 -Windows` or `./build.ps1 -Linux` (WSL). Both build the C# providers with .NET 10; add `-SkipCsharp` for a native-only build. Packages are in `dist/windows/`, `dist/linux/`, `dist/CounterStrikeSharp/` and `dist/SwiftlyS2/`.
