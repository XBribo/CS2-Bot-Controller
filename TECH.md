# BotController Developer Guide

Build BotController or consume its shared C# API. For installation and console/chat commands, see [README.md](README.md).

- [Build](#build)
- [C# Integration](#c-integration)
- [API Reference](#api-reference)
- [Behavior and Limits](#behavior-and-limits)

## Build

Run commands from the repository root. Requirements:

- **Windows:** Visual Studio 2026 C++ build tools and CMake.
- **Linux:** `g++`, CMake, and Make.
- **C#:** .NET 10 SDK.
- **Build script:** PowerShell 7 or newer; Linux builds use WSL (`Ubuntu-24.04` by default).
- **Dependencies:** Git and recursively initialized submodules.

```bash
git submodule update --init --recursive
```

Dependencies are provided by the `cs2_sdk` submodule.

### Build Script

On a Windows host:

```powershell
./build.ps1                         # Windows, Linux (WSL), and both C# providers
./build.ps1 -Windows                # Windows and both C# providers
./build.ps1 -Linux                  # Linux (WSL) and both C# providers
./build.ps1 -Windows -SkipCsharp    # Windows only
```

Add `-Clean` for a clean rebuild. Metamod and C# packages are separate:

| Output | Contents |
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

Build the provider for the framework you use; each also builds the API contract:

```bash
dotnet build csharp/BotControllerImpl/BotControllerImpl.csproj -c Release
dotnet build csharp/BotControllerImplSW2/BotControllerImplSW2.csproj -c Release
```

CMake output is under `<build-directory>/package/addons/`. Use the script to assemble the C# packages.

## C# Integration

The Metamod plugin and C# provider must use the same ABI, currently **22**. Both providers expose `IBotControllerApi` under `botcontroller:api`.

Reference the matching `BotControllerApi.dll` in your consumer project. Adjust `HintPath` to its local location; `Private=false` avoids deploying a second contract with the consumer.

```xml
<ItemGroup>
  <Reference Include="BotControllerApi">
    <HintPath>libs/BotControllerApi.dll</HintPath>
    <Private>false</Private>
  </Reference>
</ItemGroup>
```

### CounterStrikeSharp

The contract is deployed to `addons/counterstrikesharp/shared/BotControllerApi/BotControllerApi.dll`. Declare the capability in your consumer; referencing the provider implementation is not required.

```csharp
using BotControllerApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Capabilities;

public class BotControllerConsumer : BasePlugin
{
    public override string ModuleName => "BotController Consumer";
    public override string ModuleVersion => "1.0.0";
    public override string ModuleAuthor => "Example";
    public override string ModuleDescription => "Resolves the BotController API";

    private static readonly PluginCapability<IBotControllerApi> BotController = new("botcontroller:api");

    // Resolve the shared API after its provider has registered the capability.
    public override void OnAllPluginsLoaded(bool hotReload)
    {
        var api = BotController.Get();
        if (api == null) return;

        Server.PrintToConsole($"BotController ABI: {api.AbiVersion}");
    }
}
```

### SwiftlyS2

The exported contract is in `addons/swiftlys2/plugins/BotControllerImplSW2/resources/exports/BotControllerApi.dll`. In an existing SwiftlyS2 plugin class, resolve it during `UseSharedInterface`:

```csharp
using BotControllerApi;
using SwiftlyS2.Shared;

// Members of your existing SwiftlyS2 plugin class.
private IBotControllerApi? _bots;

// Resolve the provider's exported contract through the framework interface manager.
public override void UseSharedInterface(IInterfaceManager interfaceManager)
{
    _bots = interfaceManager.TryGetSharedInterface<IBotControllerApi>(
        "botcontroller:api", out var api) ? api : null;
}
```

Check for an unavailable API before calling it. SwiftlyS2 only registers the interface when the Metamod plugin is available and ABI-compatible.

## API Reference

Exact signatures are in [IBotControllerApi.cs](csharp/BotControllerApi/IBotControllerApi.cs); replay structures and lock enums are in [Types.cs](csharp/BotControllerApi/Types.cs). Slots are engine player slots, not user IDs or entity indices.

| Area | Members |
| --- | --- |
| Compatibility | `AbiVersion` |
| Locks | `Lock`, `Unlock`, `UnlockAll`, `IsLocked`, `GetWeaponLock` |
| Recording | `StartRecord`, `StopRecord`, `RecordedTickCount`, `GetRecordedMotion`, `GetRecordedMotionExtended` |
| Replay data | `LoadReplay`, `TransferRecordingToReplay`, `SetReplayPawn` |
| Replay control | `StartReplay`, `StopReplay`, `ReplayCursor`, `ReplayTotal`, `IsReplaying`, `TryGetReplayTick` |
| Weapons | `SwitchBotWeapon`, `BotActiveWeaponDef` |
| Button injection | `InjectUsercmd`, `CancelUsercmdInjection` |
| Analog movement | `StartUsercmdMovement`, `UpdateUsercmdMovement`, `CancelUsercmdMovement` |
| Button suppression | `SuppressUsercmd`, `StartUsercmdSuppression`, `CancelUsercmdSuppression` |
| Bot profile | `GetBotProfile` |
| Buy plans | `SetBuyPlan`, `SetBuySkip`, `ClearBuyPlan`, `ClearAllBuyPlans`, `BuyPlanItemCount` |
| Voice | `CanSendVoice`, `GetVoiceStatus`, `SendVoiceFrame` |

## Behavior and Limits

### Locks

- `Lock(slot, LockKind.Aim)` blocks `CCSBot::Upkeep`; other AI updates can continue.
- `Lock(slot, LockKind.All)` blocks both `CCSBot::Update` and `CCSBot::Upkeep`. This stops normal AI control, not the physics simulation.
- `Lock(slot, LockTarget.SlotN)` pins a weapon slot. Query it with `GetWeaponLock`; use `IsLocked` for `All` and `Aim`.

| Target | Engine slot | Weapon |
| --- | --- | --- |
| `Slot1` | `0` | Primary |
| `Slot2` | `1` | Pistol |
| `Slot3` | `2` | Knife / Zeus |
| `Slot4` | `3` | Grenades |
| `Slot5` | `4` | C4 |

### Record and Replay

1. Register the source's live pawn pointer with `SetReplayPawn`, then call `StartRecord`.
2. Call `StopRecord` before retrieving or transferring the recording.
3. Use `TransferRecordingToReplay` for an in-memory transfer, or `GetRecordedMotionExtended` and `LoadReplay` to include aligned command frames.
4. Register the destination bot's current pawn with `SetReplayPawn`, then call `StartReplay`.
5. Use `StopReplay` to stop playback. Register the current pawn again before restarting.

Check every operation's result. Pawn pointers must belong to the live player, not a previous spawn. Record and replay at the same tickrate; the bundled managed recording workflow assumes 64 ticks per second.

Active replay suppresses normal bot AI without requiring a separate `All` lock. Stopping replay does not clear independent locks. Preserve the contract's struct layouts and use matching Metamod and C# builds.

### Usercmd and Other Controls

- Button masks use engine `IN_*` values, such as CounterStrikeSharp's `PlayerButtons.Attack`.
- `InjectUsercmd(slot, mask, durationMs)` returns a positive cancellation token or `-1`. Zero duration presses for one command and releases on the next; a positive duration starts from the first injected command.
- Persistent movement and suppression also return positive tokens or `-1`; cancel them with the corresponding API. Movement values are clamped to `[-1, 1]`.
- Input controls require the usercmd hook and reject requests during replay. Starting replay clears existing injected input controls.
- `BuyPlanItemCount` returns `-1` for no plan, `0` for skip/empty, or a positive alias count.
- Voice requires encoded Opus frames, not raw PCM. Check `CanSendVoice` or `GetVoiceStatus` before sending; the latter returns `0` when ready.
