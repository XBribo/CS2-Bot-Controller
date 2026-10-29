// SwiftlyS2 recording provider and shared API.

using System.IO;
using System.Text.Json;
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

    private const string InterfaceKey = "botcontroller:api";
    private RecordingSessions? _sessions;
    private RecordingSessions Sessions => _sessions ??= new(
        RecordingsDir, RegisterPawnForSlot, NotifyPlayer,
        message => Logger.LogError("{Message}", message));
    private bool _bridgeChecked;
    private bool _bridgeAvailable;
    private CommandPermissionsConfig _commandPermissions = new(true);

    // ---- Lifecycle ----

    // Registers the API when the bridge is ready.
    public override void ConfigureSharedInterface(IInterfaceManager interfaceManager)
    {
        if (!EnsureBridgeAvailable()) return;

        // Publish the botcontroller:api shared interface.
        interfaceManager.AddSharedInterface<IBotControllerApi, BotControllerApiImpl>(
            InterfaceKey, new BotControllerApiImpl());
    }

    // Registers file-job callbacks.
    public override void Load(bool _)
    {
        Logger.LogInformation("{Version}, built {BuildTime}", PluginBuildInfo.DisplayVersion, PluginBuildInfo.BuildTime);
        LoadCommandPermissions();
        if (!EnsureBridgeAvailable()) return;

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
            Sessions.Drain();
            _bridgeAvailable = false;
        }
    }

    // Resets Pawn registrations on map start.
    private void OnMapLoad(IOnMapLoadEvent _) => ResetSessions();

    // Invalidates jobs before entity reuse.
    private void OnMapUnload(IOnMapUnloadEvent _) => ResetSessions();

    // Invalidates sessions before slot reuse.
    private void OnDisconnect(IOnClientDisconnectedEvent @event)
    {
        if (!_bridgeAvailable) return;
        int slot = @event.PlayerId;
        Sessions.Disconnect(slot);
    }

    // Pins pending saves to stopped buffers.
    private void ResetSessions() => Sessions.Reset();

    private void LoadCommandPermissions()
    {
        _commandPermissions = new(true);
        try
        {
            _commandPermissions = CommandPermissionsConfig.Load(
                Path.Combine(Core.GameDirectory, "addons", "BotController", "config.json"));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            Logger.LogWarning("Command permission config invalid or unreadable: using restricted defaults");
        }
    }

    private bool CanExecuteReplayCommand(ICommandContext context, IPlayer player)
    {
        if (!_commandPermissions.RequirePermissions ||
            Core.Permission.PlayerHasPermission(player.SteamID, _commandPermissions.Permission)) return true;

        context.Reply(Tag($"Permission required: {_commandPermissions.Permission}"));
        return false;
    }

    // ---- Helpers ----

    // Completes file jobs on the game thread.
    private void OnTick() => Sessions.Tick();

    // Reports job results to the requesting player.
    private void NotifyPlayer(int slot, ulong steamId, string message)
    {
        var player = Core.PlayerManager.GetPlayer(slot);
        if (player is { IsValid: true } && player.SteamID == steamId)
            player.SendMessage(MessageType.Chat, Tag(message));
    }

    // Returns the plugin-local recordings directory.
    private string RecordingsDir => Path.Combine(Core.PluginPath, "recordings");

    // Formats the reply tag.
    private static string Tag(string msg) =>
        $"{Helper.ChatColors.Green}[BotController]{Helper.ChatColors.Default} {msg}".Colored();

    // Registers the live Pawn; reports the failed stage.
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

    // Initializes the bridge once.
    private bool EnsureBridgeAvailable()
    {
        if (_bridgeChecked) return _bridgeAvailable;

        _bridgeChecked = true;
        try
        {
            ResetSessions();
            _bridgeAvailable = true;
        }
        catch (Exception ex)
        {
            Logger.LogWarning("Bridge unavailable: {Reason}", ex.Message);
        }
        return _bridgeAvailable;
    }

    // ---- Chat Commands ----

    // Starts recording the requesting player.
    [Command("record")]
    public void OnRecord(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;
        if (context.Args.Length > 1)
        {
            context.Reply(Tag("Usage: !record [fileName]"));
            return;
        }
        string? fileName = context.Args.Length >= 1 ? context.Args[0] : null;
        context.Reply(Tag(Sessions.StartRecord(player.Slot, player.SteamID, fileName)));
    }

    // Stops and saves the recording.
    [Command("stoprecord")]
    public void OnStopRecord(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;
        context.Reply(Tag(Sessions.StopRecord(player.Slot, player.SteamID)));
    }

    // Loads a recording for the target slot.
    [Command("replay")]
    public void OnReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;
        if (!CanExecuteReplayCommand(context, player)) return;
        if (context.Args.Length is < 1 or > 2 || !int.TryParse(context.Args[0], out int slot))
        {
            context.Reply(Tag("Usage: !replay <slot> [fileName]"));
            return;
        }
        string? fileName = context.Args.Length >= 2 ? context.Args[1] : null;
        context.Reply(Tag(Sessions.StartReplay(slot, fileName, player.Slot, player.SteamID)));
    }

    // Stops replay on the target slot.
    [Command("stopreplay")]
    public void OnStopReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (!_bridgeAvailable || player == null || !player.IsValid) return;
        if (!CanExecuteReplayCommand(context, player)) return;
        if (context.Args.Length < 1 || !int.TryParse(context.Args[0], out int slot)) return;
        context.Reply(Tag(Sessions.StopReplay(slot)));
    }
}
