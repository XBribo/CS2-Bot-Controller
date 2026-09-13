// CounterStrikeSharp plugin: record a player's per-tick input and replay it on
// Chat commands:
//   !record [fileName] / !stoprecord  capture your own input, save to disk
//   !replay <botSlot> [fileName]      play a recording back on a bot
//   !stopreplay <botSlot>        stop a bot's replay

using System.IO;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Utils;

using BotControllerApi;

namespace BotControllerImpl;

public partial class BotControllerPlugin : BasePlugin
{
    public override string ModuleName => "BotControllerImpl";
    public override string ModuleVersion => PluginBuildInfo.DisplayVersion;
    public override string ModuleAuthor => "XBribo";
    public override string ModuleDescription =>
        "Record & Replay and Control CS2 bots.";

    // Record and replay must share a tickrate; adjust if your server differs.
    private const int Tickrate = 64;

    private readonly ReplayDriver _driver = new();
    private readonly Dictionary<int, string> _recordingFiles = new();
    private BotControllerApiImpl? _api;

    // Loads the managed plugin and publishes its shared API
    public override void Load(bool hotReload)
    {
        Server.PrintToConsole($"[BotController] {PluginBuildInfo.DisplayVersion}, built {PluginBuildInfo.BuildTime}");
        if (!BotController.TryGetAbiVersion(out int abiVersion))
        {
            Server.PrintToConsole("[BotController] BotController native module is unavailable; disabled.");
            return;
        }

        if (!BotController.IsCompatible())
        {
            Server.PrintToConsole($"[BotController] BotController ABI mismatch; got {abiVersion}, expected 21.");
            return;
        }

        _nativeCompatible = BotController.RuntimePrepared;
        if (!_nativeCompatible)
            Server.PrintToConsole("[BotController] BotController runtime is not prepared; runtime controls are unavailable.");

        // Publish the cross-plugin API
        // Consumers: BotControllerCapability.Cap.Get().
        _api = new BotControllerApiImpl(this);
        Capabilities.RegisterPluginCapability(
            BotControllerCapability.Cap, () => _api!);

        Directory.CreateDirectory(RecordingsDir);
        BotController.SetRuntimeEnabled(false);
    }

    // Clears native and managed runtime state during plugin unload
    public override void Unload(bool hotReload)
    {
        DisableManagedRuntime();
        if (BotController.IsCompatible())
            BotController.SetRuntimeEnabled(false);
        _api = null;
    }

    private string RecordingsDir => Path.Combine(ModuleDirectory, "recordings");
    // Resolves an optional recording name to a safe plugin-local JSON path
    private bool TryGetRecordingFile(string? fileName, ulong steamId, out string file)
    {
        string name = string.IsNullOrWhiteSpace(fileName)
            ? steamId.ToString()
            : fileName;

        if (name != Path.GetFileName(name) ||
            name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
        {
            file = string.Empty;
            return false;
        }

        if (name.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
            name = name[..^5];

        if (string.IsNullOrWhiteSpace(name))
        {
            file = string.Empty;
            return false;
        }

        file = Path.Combine(RecordingsDir, $"{name}.json");
        return true;
    }

    // Find a connected player/bot by its slot, or null.
    private static CCSPlayerController? ControllerForSlot(int slot)
    {
        foreach (var p in Utilities.GetPlayers())
            if (p.Slot == slot && p.IsValid) return p;
        return null;
    }

    // Validates a bot against both CounterStrikeSharp and the native hook cache.
    internal static bool TryGetLiveBot(int slot, out CCSPlayerController? bot)
    {
        bot = ControllerForSlot(slot);
        return bot is { IsValid: true, IsBot: true } &&
               BotController.IsLiveBotSlot(slot);
    }

    // Registers the live bot pawn pointer required by the current native replay path.
    private static bool RegisterReplayPawnForSlot(int slot)
    {
        if (!TryGetLiveBot(slot, out CCSPlayerController? player) || player is not { IsValid: true, PlayerPawn: { IsValid: true } })
            return false;

        var pawn = player.PlayerPawn;
        if (pawn is not { IsValid: true, Value.IsValid: true })
            return false;

        return BotController.SetReplayPawn(slot, pawn.Value.Handle);
    }

    // Starts recording under the optional file name
    [ConsoleCommand("css_record", "Start recording: !record [fileName]")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (!RuntimeEnabled)
        {
            cmd.ReplyToCommand("[BotController] Runtime is currently disabled.");
            return;
        }
        string? fileName = cmd.ArgCount >= 2 ? cmd.GetArg(1) : null;
        if (cmd.ArgCount > 2 ||
            !TryGetRecordingFile(fileName, player.SteamID, out string file))
        {
            cmd.ReplyToCommand("[BotController] Usage: !record [fileName]");
            return;
        }
        if (!BotController.StartRecord(player.Slot))
        {
            cmd.ReplyToCommand("[BotController] Failed to start recording.");
            return;
        }
        BeginProjectileRecording(player.Slot);
        _recordingFiles[player.Slot] = file;
        cmd.ReplyToCommand("[BotController] Recording. Use !stoprecord to finish.");
    }

    // Stops recording and saves it under the selected file name
    [ConsoleCommand("css_stoprecord", "Stop recording and save to disk")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        ReplayProjectileEvent[] projectiles = FinishProjectileRecording(player.Slot);
        BotController.StopRecord(player.Slot);

        if (!_recordingFiles.Remove(player.Slot, out string? file) &&
            !TryGetRecordingFile(null, player.SteamID, out file))
            return;

        int saved = MotionStore.SaveToFile(player.Slot, file, Tickrate, projectiles);
        cmd.ReplyToCommand(saved > 0
            ? $"[BotController] Saved {saved} ticks."
            : "[BotController] Nothing recorded.");
    }

    // Loads the optional recording file and replays it on a bot
    [ConsoleCommand("css_replay", "Replay a recording: !replay <botSlot> [fileName]")]
    [CommandHelper(minArgs: 1, usage: "<botSlot> [fileName]", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (!RuntimeEnabled)
        {
            cmd.ReplyToCommand("[BotController] Runtime is currently disabled.");
            return;
        }
        string? fileName = cmd.ArgCount >= 3 ? cmd.GetArg(2) : null;
        if (cmd.ArgCount > 3 ||
            !int.TryParse(cmd.GetArg(1), out int botSlot) ||
            !TryGetRecordingFile(fileName, player.SteamID, out string file))
        {
            cmd.ReplyToCommand("[BotController] Usage: !replay <botSlot> [fileName]");
            return;
        }
        if (!File.Exists(file))
        {
            cmd.ReplyToCommand("[BotController] No recording found. Use !record first.");
            return;
        }

        MotionRecording rec = MotionStore.LoadFromFile(file);
        if (rec.Ticks.Length == 0)
        {
            cmd.ReplyToCommand("[BotController] Recording is empty.");
            return;
        }
        if (rec.Tickrate != Tickrate)
            cmd.ReplyToCommand($"[BotController] WARN tickrate mismatch: recorded {rec.Tickrate}, server {Tickrate}.");

        if (!TryGetLiveBot(botSlot, out _))
        {
            cmd.ReplyToCommand("[BotController] Target slot is not a live bot.");
            return;
        }

        PrepareProjectileReplay(botSlot, rec);
        if (BotController.LoadReplayExtended(
                botSlot,
                rec.Ticks,
                rec.Subticks,
                rec.Commands ?? Array.Empty<ReplayCommandFrame>(),
                Array.Empty<ReplayMovementExtra>()) &&
            RegisterReplayPawnForSlot(botSlot) &&
            BotController.StartReplay(botSlot))
        {
            _driver.Track(botSlot);
            cmd.ReplyToCommand($"[BotController] Replaying on bot slot {botSlot}.");
        }
        else
        {
            ClearProjectileReplay(botSlot);
            cmd.ReplyToCommand("[BotController] Failed to start replay.");
        }
    }

    // Stops replay on the selected bot slot
    [ConsoleCommand("css_stopreplay", "Stop a bot's replay: !stopreplay <botSlot>")]
    [CommandHelper(minArgs: 1, usage: "<botSlot>", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (!int.TryParse(cmd.GetArg(1), out int botSlot)) return;
        BotController.StopReplay(botSlot);
        _driver.Release(botSlot);
        ClearProjectileReplay(botSlot);
        cmd.ReplyToCommand($"[BotController] Stopped replay on bot slot {botSlot}.");
    }
}
