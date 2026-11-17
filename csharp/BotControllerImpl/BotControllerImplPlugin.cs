// CounterStrikeSharp recording provider and shared API.

using System.IO;
using System.Text.Json;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Admin;
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

    private RecordingSessions? _sessions;
    private RecordingSessions Sessions => _sessions ??= new(
        RecordingsDir, RegisterPawnForSlot, NotifyPlayer,
        message => Server.PrintToConsole($"[BotController] {message}"));
    private BotControllerApiImpl? _api;
    private CommandPermissionsConfig _commandPermissions = new(true);

    // Loads the provider and publishes the API.
    public override void Load(bool hotReload)
    {
        Server.PrintToConsole($"[BotController] {PluginBuildInfo.DisplayVersion}, built {PluginBuildInfo.BuildTime}");
        LoadCommandPermissions();
        ResetSessions();

        Directory.CreateDirectory(RecordingsDir);
        RegisterListener<Listeners.OnTick>(OnTick);
        RegisterListener<Listeners.OnMapStart>(_ => ResetSessions());
        RegisterListener<Listeners.OnMapEnd>(ResetSessions);
        RegisterListener<Listeners.OnClientDisconnect>(OnDisconnect);
        _api = new BotControllerApiImpl();
        try
        {
            BotControllerProvider.Attach(_api,
                supplier => Capabilities.RegisterPluginCapability(BotControllerCapability.Cap, supplier));
        }
        catch
        {
            _api.Dispose();
            _api = null;
            throw;
        }
    }

    // Drains native buffer readers before unload.
    public override void Unload(bool hotReload)
    {
        BotControllerProvider.Detach(_api);
        _api?.Dispose();
        _api = null;
        _sessions?.Drain();
        _sessions = null;
    }

    private void LoadCommandPermissions()
    {
        _commandPermissions = new(true);
        try
        {
            _commandPermissions = CommandPermissionsConfig.Load(
                Path.Combine(Server.GameDirectory, "csgo", "addons", "BotController", "config.json"));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            Server.PrintToConsole("[BotController] Permission config failed; using restricted defaults");
        }
    }

    private bool CanExecuteReplayCommand(CCSPlayerController player, CommandInfo cmd)
    {
        if (!_commandPermissions.RequirePermissions) return true;
        if (player.AuthorizedSteamID != null &&
            (AdminManager.PlayerHasPermissions(player, "@css/root") ||
             AdminManager.PlayerHasPermissions(player, "@css/admin") ||
             AdminManager.PlayerHasPermissions(player, _commandPermissions.Permission))) return true;

        cmd.ReplyToCommand($"[BotController] permission required: {_commandPermissions.Permission}, @css/root or @css/admin");
        return false;
    }

    // Invalidates sessions before slot reuse.
    private void OnDisconnect(int slot)
    {
        if (_api == null) return;
        Sessions.Disconnect(slot);
    }

    // Pins pending saves to stopped buffers.
    private void ResetSessions() => Sessions.Reset();

    // Completes file jobs on the game thread.
    private void OnTick() => Sessions.Tick();

    // Reports job results to the requesting player.
    private static void NotifyPlayer(int slot, ulong steamId, string message)
    {
        var player = ControllerForSlot(slot);
        if (player is { IsValid: true } && player.SteamID == steamId)
            player.PrintToChat($"[BotController] {message}");
    }

    private string RecordingsDir => Path.Combine(ModuleDirectory, "recordings");
    // Finds a connected player or Bot by slot.
    private static CCSPlayerController? ControllerForSlot(int slot)
    {
        foreach (var p in Utilities.GetPlayers())
            if (p.Slot == slot && p.IsValid) return p;
        return null;
    }

    // Resolves the slot's entity directly.
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

    // Starts recording to the selected file.
    [ConsoleCommand("css_record", "Start recording: !record [fileName]")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_api == null || player == null || !player.IsValid) return;
        if (cmd.ArgCount > 2)
        {
            cmd.ReplyToCommand("[BotController] Usage: !record [fileName]");
            return;
        }
        string? fileName = cmd.ArgCount >= 2 ? cmd.GetArg(1) : null;
        cmd.ReplyToCommand($"[BotController] {Sessions.StartRecord(player.Slot, player.SteamID, fileName)}");
    }

    // Stops and saves the recording.
    [ConsoleCommand("css_stoprecord", "Stop recording and save to disk")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_api == null || player == null || !player.IsValid) return;
        cmd.ReplyToCommand($"[BotController] {Sessions.StopRecord(player.Slot, player.SteamID)}");
    }

    // Loads a recording for the target slot.
    [ConsoleCommand("css_replay", "Replay a recording: !replay <slot> [fileName]")]
    [CommandHelper(minArgs: 1, usage: "<slot> [fileName]", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_api == null || player == null || !player.IsValid) return;
        if (!CanExecuteReplayCommand(player, cmd)) return;
        if (cmd.ArgCount > 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        {
            cmd.ReplyToCommand("[BotController] Usage: !replay <slot> [fileName]");
            return;
        }
        string? fileName = cmd.ArgCount >= 3 ? cmd.GetArg(2) : null;
        cmd.ReplyToCommand($"[BotController] {Sessions.StartReplay(slot, fileName, player.Slot, player.SteamID)}");
    }

    // Stops the target slot's replay.
    [ConsoleCommand("css_stopreplay", "Stop a replay: !stopreplay <slot>")]
    [CommandHelper(minArgs: 1, usage: "<slot>", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_api == null || player == null || !player.IsValid) return;
        if (!CanExecuteReplayCommand(player, cmd)) return;
        if (!int.TryParse(cmd.GetArg(1), out int slot)) return;
        cmd.ReplyToCommand($"[BotController] {Sessions.StopReplay(slot)}");
    }
}
