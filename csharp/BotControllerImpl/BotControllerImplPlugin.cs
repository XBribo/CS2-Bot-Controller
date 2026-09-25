// CounterStrikeSharp plugin: record a player's per-tick input and replay it on
// Chat commands:
//   !record [fileName] / !stoprecord  capture your own input, save to disk
//   !replay <botSlot> [fileName]      play a recording back on a bot
//   !stopreplay <botSlot>        stop a bot's replay

using System.IO;
using System.Collections.Concurrent;
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
    private readonly HashSet<int> _savingSlots = new();
    private readonly Dictionary<int, object> _loadingSlots = new();
    private readonly HashSet<int> _cancelledLoads = new();
    private readonly ConcurrentQueue<Action> _completedJobs = new();

    // Loads the managed plugin and publishes its shared API
    public override void Load(bool hotReload)
    {
        Server.PrintToConsole($"[BotController] {PluginBuildInfo.DisplayVersion}, built {PluginBuildInfo.BuildTime}");
        if (!BotController.IsCompatible())
        {
            Server.PrintToConsole("[BotController] BotController ABI mismatch; disabled.");
            return;
        }

        // Publish the cross-plugin API
        // Consumers: BotControllerCapability.Cap.Get().
        Capabilities.RegisterPluginCapability(
            BotControllerCapability.Cap, () => new BotControllerApiImpl());

        Directory.CreateDirectory(RecordingsDir);
        RegisterListener<Listeners.OnTick>(OnTick);
    }

    // Applies completed file jobs on the game thread before replay bookkeeping.
    private void OnTick()
    {
        while (_completedJobs.TryDequeue(out Action? completion)) completion();
        _driver.Tick();
    }

    // Reports an asynchronous result only to the player who issued the command.
    private static void NotifyPlayer(int slot, ulong steamId, string message)
    {
        var player = ControllerForSlot(slot);
        if (player is { IsValid: true } && player.SteamID == steamId)
            player.PrintToChat($"[BotController] {message}");
    }

    private string RecordingsDir => Path.Combine(ModuleDirectory, "recordings");
    // Resolves an optional recording name to a safe plugin-local compressed JSON path
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

        if (name.EndsWith(".json.br", StringComparison.OrdinalIgnoreCase))
            name = name[..^8];

        if (string.IsNullOrWhiteSpace(name))
        {
            file = string.Empty;
            return false;
        }

        file = Path.Combine(RecordingsDir, $"{name}.json.br");
        return true;
    }

    // Find a connected player/bot by its slot, or null.
    private static CCSPlayerController? ControllerForSlot(int slot)
    {
        foreach (var p in Utilities.GetPlayers())
            if (p.Slot == slot && p.IsValid) return p;
        return null;
    }

    // Registers the live pawn before the first recording or replay frame.
    private static bool RegisterPawnForSlot(int slot)
    {
        var player = ControllerForSlot(slot);
        if (player is not { IsValid: true } ||
            player.PlayerPawn is not { IsValid: true, Value.IsValid: true })
            return false;

        return BotController.SetReplayPawn(slot, player.PlayerPawn.Value.Handle);
    }

    // Starts recording under the optional file name
    [ConsoleCommand("css_record", "Start recording: !record [fileName]")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (_savingSlots.Contains(player.Slot))
        {
            cmd.ReplyToCommand("[BotController] Previous recording is still saving.");
            return;
        }
        string? fileName = cmd.ArgCount >= 2 ? cmd.GetArg(1) : null;
        if (cmd.ArgCount > 2 ||
            !TryGetRecordingFile(fileName, player.SteamID, out string file))
        {
            cmd.ReplyToCommand("[BotController] Usage: !record [fileName]");
            return;
        }
        if (!RegisterPawnForSlot(player.Slot) || !BotController.StartRecord(player.Slot))
        {
            cmd.ReplyToCommand("[BotController] Failed to start recording.");
            return;
        }
        _recordingFiles[player.Slot] = file;
        cmd.ReplyToCommand("[BotController] Recording. Use !stoprecord to finish.");
    }

    // Stops recording and saves it under the selected file name
    [ConsoleCommand("css_stoprecord", "Stop recording and save to disk")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (_savingSlots.Contains(player.Slot))
        {
            cmd.ReplyToCommand("[BotController] Recording is still saving.");
            return;
        }
        BotController.StopRecord(player.Slot);

        if (!_recordingFiles.Remove(player.Slot, out string? file) &&
            !TryGetRecordingFile(null, player.SteamID, out file))
            return;

        int slot = player.Slot;
        ulong steamId = player.SteamID;
        _savingSlots.Add(slot);
        cmd.ReplyToCommand("[BotController] Saving recording...");
        _ = Task.Run(() =>
        {
            try
            {
                int saved = MotionStore.SaveToFile(slot, file, Tickrate);
                _completedJobs.Enqueue(() =>
                {
                    _savingSlots.Remove(slot);
                    NotifyPlayer(slot, steamId, saved > 0 ? $"Saved {saved} ticks." : "Nothing recorded.");
                });
            }
            catch (Exception ex)
            {
                _completedJobs.Enqueue(() =>
                {
                    _savingSlots.Remove(slot);
                    Server.PrintToConsole($"[BotController] Save failed: {ex}");
                    NotifyPlayer(slot, steamId, "Recording save failed.");
                });
            }
        });
    }

    // Loads the optional recording file and replays it on a bot
    [ConsoleCommand("css_replay", "Replay a recording: !replay <botSlot> [fileName]")]
    [CommandHelper(minArgs: 1, usage: "<botSlot> [fileName]", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
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

        if (_loadingSlots.ContainsKey(botSlot))
        {
            cmd.ReplyToCommand("[BotController] Replay is still loading on that slot.");
            return;
        }
        int requesterSlot = player.Slot;
        ulong requesterSteamId = player.SteamID;
        object token = new();
        _loadingSlots.Add(botSlot, token);
        cmd.ReplyToCommand("[BotController] Loading replay...");
        _ = Task.Run(() =>
        {
            try
            {
                MotionRecording rec = MotionStore.LoadFromFile(file);
                bool loaded = rec.Ticks.Length > 0 && BotController.LoadReplay(
                    botSlot, rec.Ticks, rec.Subticks, rec.Commands, Array.Empty<ReplayMovementExtra>());
                _completedJobs.Enqueue(() =>
                {
                    if (!_loadingSlots.TryGetValue(botSlot, out object? current) || !ReferenceEquals(current, token)) return;
                    _loadingSlots.Remove(botSlot);
                    if (_cancelledLoads.Remove(botSlot)) return;
                    if (rec.Tickrate != Tickrate)
                        NotifyPlayer(requesterSlot, requesterSteamId, $"WARN tickrate mismatch: recorded {rec.Tickrate}, server {Tickrate}.");
                    if (loaded && RegisterPawnForSlot(botSlot) && BotController.StartReplay(botSlot))
                    {
                        _driver.Track(botSlot);
                        NotifyPlayer(requesterSlot, requesterSteamId, $"Replaying on bot slot {botSlot}.");
                    }
                    else NotifyPlayer(requesterSlot, requesterSteamId, "Failed to start replay.");
                });
            }
            catch (Exception ex)
            {
                _completedJobs.Enqueue(() =>
                {
                    if (!_loadingSlots.TryGetValue(botSlot, out object? current) || !ReferenceEquals(current, token)) return;
                    _loadingSlots.Remove(botSlot);
                    if (_cancelledLoads.Remove(botSlot)) return;
                    Server.PrintToConsole($"[BotController] Replay load failed: {ex}");
                    NotifyPlayer(requesterSlot, requesterSteamId, "Replay load failed.");
                });
            }
        });
    }

    // Stops replay on the selected bot slot
    [ConsoleCommand("css_stopreplay", "Stop a bot's replay: !stopreplay <botSlot>")]
    [CommandHelper(minArgs: 1, usage: "<botSlot>", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (!int.TryParse(cmd.GetArg(1), out int botSlot)) return;
        if (_loadingSlots.ContainsKey(botSlot)) _cancelledLoads.Add(botSlot);
        BotController.StopReplay(botSlot);
        _driver.Release(botSlot);
        cmd.ReplyToCommand($"[BotController] Stopped replay on bot slot {botSlot}.");
    }
}
