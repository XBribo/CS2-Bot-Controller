// CounterStrikeSharp plugin: record a player's per-tick input and replay it on
// Chat commands:
//   !record / !stoprecord        capture your own input, save to disk
//   !replay <botSlot> [loop]     play your recording back on a bot
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

public class BotControllerPlugin : BasePlugin
{
    public override string ModuleName => "BotControllerImpl";
    public override string ModuleVersion => "1.1.6";
    public override string ModuleAuthor => "XBribo";
    public override string ModuleDescription =>
        "Record a player's movement and replay it on a bot.";

    // Record and replay must share a tickrate; adjust if your server differs.
    private const int Tickrate = 64;

    private readonly ReplayDriver _driver = new();

    public override void Load(bool hotReload)
    {
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
        RegisterListener<Listeners.OnTick>(_driver.Tick);
    }

    private string RecordingsDir => Path.Combine(ModuleDirectory, "recordings");
    private string FileFor(CCSPlayerController p) =>
        Path.Combine(RecordingsDir, $"{p.SteamID}.json");

    // Find a connected player/bot by its slot, or null.
    private static CCSPlayerController? ControllerForSlot(int slot)
    {
        foreach (var p in Utilities.GetPlayers())
            if (p.Slot == slot && p.IsValid) return p;
        return null;
    }

    [ConsoleCommand("css_record", "Start recording your per-tick movement")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (!BotController.StartRecord(player.Slot))
        {
            cmd.ReplyToCommand("[BotController] Failed to start recording.");
            return;
        }
        cmd.ReplyToCommand("[BotController] Recording. Use !stoprecord to finish.");
    }

    [ConsoleCommand("css_stoprecord", "Stop recording and save to disk")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopRecord(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        BotController.StopRecord(player.Slot);

        int saved = MotionStore.SaveToFile(player.Slot, FileFor(player), Tickrate);
        cmd.ReplyToCommand(saved > 0
            ? $"[BotController] Saved {saved} ticks."
            : "[BotController] Nothing recorded.");
    }

    [ConsoleCommand("css_replay", "Replay your recording on a bot: !replay <botSlot> [loop]")]
    [CommandHelper(minArgs: 1, usage: "<botSlot> [loop]", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (!int.TryParse(cmd.GetArg(1), out int botSlot))
        {
            cmd.ReplyToCommand("[BotController] Usage: !replay <botSlot> [loop]");
            return;
        }
        bool loop = cmd.ArgCount >= 3 && cmd.GetArg(2) == "loop";
        string file = FileFor(player);
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

        // Freeze the bot's AI so it doesn't fight the replayed motion.
        BotController.Lock(botSlot, LockKind.All);

        if (BotController.LoadReplay(botSlot, rec.Ticks, rec.Subticks) &&
            BotController.StartReplay(botSlot, loop))
        {
            _driver.Track(botSlot);
            cmd.ReplyToCommand($"[BotController] Replaying on bot slot {botSlot}{(loop ? " (loop)" : "")}.");
        }
        else
        {
            BotController.Unlock(botSlot, LockKind.All);
            cmd.ReplyToCommand("[BotController] Failed to start replay.");
        }
    }

    [ConsoleCommand("css_stopreplay", "Stop a bot's replay: !stopreplay <botSlot>")]
    [CommandHelper(minArgs: 1, usage: "<botSlot>", whoCanExecute: CommandUsage.CLIENT_ONLY)]
    public void OnStopReplay(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !player.IsValid) return;
        if (!int.TryParse(cmd.GetArg(1), out int botSlot)) return;
        BotController.StopReplay(botSlot);
        _driver.Release(botSlot);
        cmd.ReplyToCommand($"[BotController] Stopped replay on bot slot {botSlot}.");
    }
}
