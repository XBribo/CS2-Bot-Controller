// SwiftlyS2 plugin: record a player's per-tick input and replay it on a bot.
// Commands:
//   !record [fileName] / !stoprecord  capture your own input, save to disk
//   !replay <botSlot> [fileName]      play a recording back on a bot
//   !stopreplay <botSlot>        stop a bot's replay
//
// Also exposes IBotControllerApi via IInterfaceManager for cross-plugin use.

using System.IO;
using System.Collections.Concurrent;
using Microsoft.Extensions.Logging;
using SwiftlyS2.Shared;
using SwiftlyS2.Shared.Commands;
using SwiftlyS2.Shared.Events;
using SwiftlyS2.Shared.Players;
using SwiftlyS2.Shared.Plugins;

using BotControllerApi;

namespace BotControllerImplSW2;

[PluginMetadata(
    Id = "botcontroller.sw2",
    Version = PluginBuildInfo.Version,
    Name = "BotController",
    Author = "XBribo & nicedayzhu",
    Description = "Record & Replay and Control CS2 bots."
)]
public partial class BotControllerImplSW2Plugin(ISwiftlyCore core) : BasePlugin(core)
{
    public ILogger<BotControllerImplSW2Plugin> Logger => Core.LoggerFactory.CreateLogger<BotControllerImplSW2Plugin>();

    // Record and replay must share a tickrate; adjust if your server differs.
    private const int Tickrate = 64;
    private const string InterfaceKey = "botcontroller:api";

    private readonly Dictionary<int, (ulong SteamId, string File)> _recordingFiles = new();
    private readonly HashSet<int> _savingSlots = new();
    private readonly Dictionary<int, ulong> _loadingSlots = new();
    private readonly ConcurrentQueue<Action> _completedJobs = new();
    private readonly List<Task> _jobs = new();
    private ulong _mapGeneration;
    private bool _bridgeChecked;
    private bool _bridgeAvailable;

    // ---- Lifecycle ----

    // Registers the shared API only when the BotController bridge is usable.
    public override void ConfigureSharedInterface(IInterfaceManager interfaceManager)
    {
        if (!EnsureBridgeAvailable()) return;

        // Consumers call: interfaceManager.TryGetSharedInterface<IBotControllerApi>("botcontroller:api", out var api)
        interfaceManager.AddSharedInterface<IBotControllerApi, BotControllerApiImpl>(
            InterfaceKey, new BotControllerApiImpl());
    }

    // Registers file-job callbacks when the bridge is ready.
    public override void Load(bool _)
    {
        Logger.LogInformation("{Version}, built {BuildTime}", PluginBuildInfo.DisplayVersion, PluginBuildInfo.BuildTime);
        if (!EnsureBridgeAvailable()) return;

        ResetSessions();
        Directory.CreateDirectory(RecordingsDir);

        Core.Event.OnTick += OnTick;
        Core.Event.OnMapLoad += OnMapLoad;
        Core.Event.OnMapUnload += OnMapUnload;
        Core.Event.OnClientDisconnected += OnDisconnect;
    }

    // Removes file-job callbacks on unload.
    public override void Unload()
    {
        if (_bridgeAvailable)
        {
            Core.Event.OnTick -= OnTick;
            Core.Event.OnMapLoad -= OnMapLoad;
            Core.Event.OnMapUnload -= OnMapUnload;
            Core.Event.OnClientDisconnected -= OnDisconnect;
            ResetSessions();
            Task.WaitAll(_jobs.ToArray());
            _jobs.Clear();
            _completedJobs.Clear();
            _savingSlots.Clear();
            _bridgeAvailable = false;
        }
    }

    // Starts each map with explicit pawn registration.
    private void OnMapLoad(IOnMapLoadEvent _) => ResetSessions();

    // Invalidates jobs before entity indices can be reused.
    private void OnMapUnload(IOnMapUnloadEvent _) => ResetSessions();

    // Invalidates ownership before a disconnected slot can be reused.
    private void OnDisconnect(IOnClientDisconnectedEvent @event)
    {
        if (!_bridgeAvailable) return;
        int slot = @event.PlayerId;
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

    // ---- Helpers ----

    // Applies completed file jobs on the game thread.
    private void OnTick()
    {
        BotController.RefreshPawns();
        while (_completedJobs.TryDequeue(out Action? completion)) completion();
        _jobs.RemoveAll(job => job.IsCompleted);
    }

    // Reports an asynchronous result only to the player who issued the command.
    private void NotifyPlayer(int slot, ulong steamId, string message)
    {
        var player = Core.PlayerManager.GetPlayer(slot);
        if (player is { IsValid: true } && player.SteamID == steamId)
            player.SendMessage(MessageType.Chat, Tag(message));
    }

    // Returns the plugin-local recordings directory.
    private string RecordingsDir => Path.Combine(Core.PluginPath, "recordings");

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

    // Formats a colored chat tag for BotController replies.
    private static string Tag(string msg) =>
        $"{Helper.ChatColors.Green}[BotController]{Helper.ChatColors.Default} {msg}".Colored();

    // Registers the live pawn or returns the failed stage.
    private string? RegisterPawnForSlot(int slot)
    {
        if (slot is < 0 or >= 64) return "Slot must be 0..63.";
        var player = Core.PlayerManager.GetPlayer(slot);
        if (player is not { IsValid: true }) return $"Slot {slot}: controller unavailable.";
        var pawn = player.PlayerPawn;
        if (pawn is not { IsValid: true }) return $"Slot {slot}: pawn unavailable.";
        return BotController.SetReplayPawn(slot, ((SwiftlyS2.Shared.Natives.INativeHandle)pawn).Address)
            ? null
            : $"Slot {slot}: native pawn registration failed.";
    }

    // Returns the cached bridge state, probing once on first use.
    private bool EnsureBridgeAvailable()
    {
        if (_bridgeChecked) return _bridgeAvailable;

        _bridgeChecked = true;
        _bridgeAvailable = ProbeBridge();
        return _bridgeAvailable;
    }

    // Checks bridge availability and ABI compatibility.
    private bool ProbeBridge()
    {
        try
        {
            if (BotController.IsCompatible())
                return true;

            Logger.LogWarning("ABI mismatch: {AbiVersion}", BotController.AbiVersion);
            return false;
        }
        catch (Exception ex)
        {
            Logger.LogWarning("Bridge unavailable: {Reason}", ex.Message);
            return false;
        }
    }

    // ---- Chat Commands ----

    // Starts recording the invoking player's movement.
    [Command("record")]
    public void OnRecord(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;
        if (_savingSlots.Contains(player.Slot))
        {
            context.Reply(Tag("Previous recording is still saving."));
            return;
        }

        string? fileName = context.Args.Length >= 1 ? context.Args[0] : null;
        if (context.Args.Length > 1 ||
            !TryGetRecordingFile(fileName, player.SteamID, out string file))
        {
            context.Reply(Tag("Usage: !record [fileName]"));
            return;
        }
        if (RegisterPawnForSlot(player.Slot) is { } failure)
        {
            context.Reply(Tag(failure));
            return;
        }
        if (!BotController.StartRecord(player.Slot))
        {
            context.Reply(Tag("Failed to start recording."));
            return;
        }
        _recordingFiles[player.Slot] = (player.SteamID, file);
        context.Reply(Tag("Recording. Use !stoprecord to finish."));
    }

    // Stops recording and persists the invoking player's captured motion.
    [Command("stoprecord")]
    public void OnStopRecord(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;
        if (_savingSlots.Contains(player.Slot))
        {
            context.Reply(Tag("Recording is still saving."));
            return;
        }

        if (!_recordingFiles.TryGetValue(player.Slot, out var recording) || recording.SteamId != player.SteamID)
        {
            context.Reply(Tag("No recording."));
            return;
        }
        _recordingFiles.Remove(player.Slot);
        BotController.StopRecord(player.Slot);
        string file = recording.File;

        int slot = player.Slot;
        ulong steamId = player.SteamID;
        ulong mapGeneration = _mapGeneration;
        _savingSlots.Add(slot);
        context.Reply(Tag("Saving recording..."));
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
                    Logger.LogError("Save failed: {Reason}", ex.Message);
                    NotifyPlayer(slot, steamId, "Recording save failed.");
                });
            }
        }));
    }

    // Loads the invoking player's recording and starts replay on a target bot slot.
    [Command("replay")]
    public void OnReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;

        string? fileName = context.Args.Length >= 2 ? context.Args[1] : null;
        if (context.Args.Length is < 1 or > 2 ||
            !int.TryParse(context.Args[0], out int botSlot) ||
            !TryGetRecordingFile(fileName, player.SteamID, out string file))
        {
            context.Reply(Tag("Usage: !replay <botSlot> [fileName]"));
            return;
        }
        if (!File.Exists(file))
        {
            context.Reply(Tag("No recording found. Use !record first."));
            return;
        }

        if (_loadingSlots.ContainsKey(botSlot))
        {
            context.Reply(Tag("Replay is still loading on that slot."));
            return;
        }
        if (BotController.IsReplaying(botSlot))
        {
            context.Reply(Tag($"Slot {botSlot}: replay already running."));
            return;
        }
        if (RegisterPawnForSlot(botSlot) is { } failure)
        {
            context.Reply(Tag(failure));
            return;
        }
        ulong generation = BotController.PawnGeneration(botSlot);
        if (generation == 0) return;
        int requesterSlot = player.Slot;
        ulong requesterSteamId = player.SteamID;
        _loadingSlots.Add(botSlot, generation);
        context.Reply(Tag("Loading replay..."));
        _jobs.Add(Task.Run(() =>
        {
            try
            {
                MotionRecording rec = MotionStore.LoadFromFile(file);
                bool loaded = rec.Ticks.Length > 0 && BotController.LoadReplay(
                    botSlot, rec.Ticks, rec.Subticks, rec.Commands, generation);
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
                    Logger.LogError("Replay load failed: {Reason}", ex.Message);
                    NotifyPlayer(requesterSlot, requesterSteamId, "Replay load failed.");
                });
            }
        }));
    }

    // Stops replay on the target slot.
    [Command("stopreplay")]
    public void OnStopReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;

        if (context.Args.Length < 1 || !int.TryParse(context.Args[0], out int botSlot)) return;

        _loadingSlots.Remove(botSlot);

        BotController.StopReplay(botSlot);
        context.Reply(Tag($"Stopped replay on bot slot {botSlot}."));
    }
}
