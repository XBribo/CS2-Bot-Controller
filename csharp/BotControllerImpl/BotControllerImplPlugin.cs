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

    private readonly Dictionary<int, (ulong SteamId, string File)> _recordingFiles = new();
    private readonly HashSet<int> _savingSlots = new();
    private readonly Dictionary<int, ulong> _loadingSlots = new();
    private readonly ConcurrentQueue<Action> _completedJobs = new();
    private readonly List<Task> _jobs = new();
    private bool _bridgeReady;
    private ulong _mapGeneration;

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

        _bridgeReady = true;
        ResetSessions();
        Directory.CreateDirectory(RecordingsDir);
        RegisterListener<Listeners.OnTick>(OnTick);
        RegisterListener<Listeners.OnMapStart>(_ => ResetSessions());
        RegisterListener<Listeners.OnMapEnd>(ResetSessions);
        RegisterListener<Listeners.OnClientDisconnect>(OnDisconnect);
    }

    // Drains native buffer readers before the provider disappears.
    public override void Unload(bool hotReload)
    {
        if (!_bridgeReady) return;
        _bridgeReady = false;
        ResetSessions();
        Task.WaitAll(_jobs.ToArray());
        _jobs.Clear();
        _completedJobs.Clear();
        _savingSlots.Clear();
    }

    // Invalidates ownership before a disconnected slot can be reused.
    private void OnDisconnect(int slot)
    {
        if (!_bridgeReady) return;
        _loadingSlots.Remove(slot);
        _recordingFiles.Remove(slot);
        BotController.ResetPawn(slot);
    }

    // Keeps in-flight saves pinned to their stopped native buffers.
    private void ResetSessions()
    {
        ++_mapGeneration;
        _loadingSlots.Clear();
        _recordingFiles.Clear();
        BotController.ResetPawns();
    }

    // Applies completed file jobs on the game thread.
    private void OnTick()
    {
        BotController.RefreshPawns();
        while (_completedJobs.TryDequeue(out Action? completion)) completion();
        _jobs.RemoveAll(job => job.IsCompleted);
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

    // Resolves the slot directly without GetPlayers enumeration filters.
    private static string? RegisterPawnForSlot(int slot)
    {
        if (slot is < 0 or >= 64) return "Slot must be 0..63.";
        var player = Utilities.GetPlayerFromSlot(slot);
        if (player is not { IsValid: true }) return $"Slot {slot}: controller unavailable.";
        var pawn = player.PlayerPawn;
        if (pawn is not { IsValid: true, Value.IsValid: true }) return $"Slot {slot}: pawn unavailable.";

        return BotController.SetReplayPawn(slot, pawn.Value.Handle)
            ? null
            : $"Slot {slot}: native pawn registration failed.";
    }

    // Starts recording under the optional file name
    [ConsoleCommand("css_record", "Start recording: !record [fileName]")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!_bridgeReady || player == null || !player.IsValid) return;
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
        if (RegisterPawnForSlot(player.Slot) is { } failure)
        {
            cmd.ReplyToCommand($"[BotController] {failure}");
            return;
        }
        if (!BotController.StartRecord(player.Slot))
        {
            cmd.ReplyToCommand("[BotController] Failed to start recording.");
            return;
        }
        _recordingFiles[player.Slot] = (player.SteamID, file);
        cmd.ReplyToCommand("[BotController] Recording. Use !stoprecord to finish.");
    }

    // Stops recording and saves it under the selected file name
    [ConsoleCommand("css_stoprecord", "Stop recording and save to disk")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!_bridgeReady || player == null || !player.IsValid) return;
        if (_savingSlots.Contains(player.Slot))
        {
            cmd.ReplyToCommand("[BotController] Recording is still saving.");
            return;
        }
        if (!_recordingFiles.TryGetValue(player.Slot, out var recording) || recording.SteamId != player.SteamID)
        {
            cmd.ReplyToCommand("[BotController] No recording.");
            return;
        }
        _recordingFiles.Remove(player.Slot);
        BotController.StopRecord(player.Slot);
        string file = recording.File;

        int slot = player.Slot;
        ulong steamId = player.SteamID;
        ulong mapGeneration = _mapGeneration;
        _savingSlots.Add(slot);
        cmd.ReplyToCommand("[BotController] Saving recording...");
        _jobs.Add(Task.Run(() =>
        {
            try
            {
                int saved = MotionStore.SaveToFile(slot, file, Tickrate);
                _completedJobs.Enqueue(() =>
                {
                    _savingSlots.Remove(slot);
                    if (mapGeneration != _mapGeneration) return;
                    NotifyPlayer(slot, steamId, saved > 0 ? $"Saved {saved} ticks." : "Nothing recorded.");
                });
            }
            catch (Exception ex)
            {
                _completedJobs.Enqueue(() =>
                {
                    _savingSlots.Remove(slot);
                    if (mapGeneration != _mapGeneration) return;
                    Server.PrintToConsole($"[BotController] Save failed: {ex.Message}");
                    NotifyPlayer(slot, steamId, "Recording save failed.");
                });
            }
        }));
    }

    // Loads the optional recording file and replays it on a bot
    [ConsoleCommand("css_replay", "Replay a recording: !replay <botSlot> [fileName]")]
    [CommandHelper(minArgs: 1, usage: "<botSlot> [fileName]", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!_bridgeReady || player == null || !player.IsValid) return;
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
        if (BotController.IsReplaying(botSlot))
        {
            cmd.ReplyToCommand($"[BotController] Slot {botSlot}: replay already running.");
            return;
        }
        if (RegisterPawnForSlot(botSlot) is { } failure)
        {
            cmd.ReplyToCommand($"[BotController] {failure}");
            return;
        }
        ulong generation = BotController.PawnGeneration(botSlot);
        if (generation == 0) return;
        int requesterSlot = player.Slot;
        ulong requesterSteamId = player.SteamID;
        _loadingSlots.Add(botSlot, generation);
        cmd.ReplyToCommand("[BotController] Loading replay...");
        _jobs.Add(Task.Run(() =>
        {
            try
            {
                MotionRecording rec = MotionStore.LoadFromFile(file);
                bool loaded = rec.Ticks.Length > 0 && BotController.LoadReplay(
                    botSlot, rec.Ticks, rec.Subticks, rec.Commands, rec.InputHistory, generation);
                _completedJobs.Enqueue(() =>
                {
                    if (!_loadingSlots.TryGetValue(botSlot, out ulong current) || current != generation) return;
                    _loadingSlots.Remove(botSlot);
                    if (BotController.PawnGeneration(botSlot) != generation)
                    {
                        NotifyPlayer(requesterSlot, requesterSteamId, "Replay cancelled: pawn changed.");
                        return;
                    }
                    if (rec.Tickrate != Tickrate)
                        NotifyPlayer(requesterSlot, requesterSteamId, $"WARN tickrate mismatch: recorded {rec.Tickrate}, server {Tickrate}.");
                    if (loaded && BotController.StartReplay(botSlot))
                        NotifyPlayer(requesterSlot, requesterSteamId, $"Replaying on bot slot {botSlot}.");
                    else NotifyPlayer(requesterSlot, requesterSteamId, "Failed to start replay.");
                });
            }
            catch (Exception ex)
            {
                _completedJobs.Enqueue(() =>
                {
                    if (!_loadingSlots.TryGetValue(botSlot, out ulong current) || current != generation) return;
                    _loadingSlots.Remove(botSlot);
                    Server.PrintToConsole($"[BotController] Replay load failed: {ex.Message}");
                    NotifyPlayer(requesterSlot, requesterSteamId, "Replay load failed.");
                });
            }
        }));
    }

    // Stops replay on the selected bot slot
    [ConsoleCommand("css_stopreplay", "Stop a bot's replay: !stopreplay <botSlot>")]
    [CommandHelper(minArgs: 1, usage: "<botSlot>", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!_bridgeReady || player == null || !player.IsValid) return;
        if (!int.TryParse(cmd.GetArg(1), out int botSlot)) return;
        _loadingSlots.Remove(botSlot);
        BotController.StopReplay(botSlot);
        cmd.ReplyToCommand($"[BotController] Stopped replay on bot slot {botSlot}.");
    }
}
