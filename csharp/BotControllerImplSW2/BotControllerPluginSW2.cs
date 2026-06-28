// SwiftlyS2 plugin: record a player's per-tick input and replay it on a bot.
// Commands:
//   !record / !stoprecord        capture your own input, save to disk
//   !replay <botSlot> [loop]     play your recording back on a bot
//   !stopreplay <botSlot>        stop a bot's replay
//
// Also exposes IBotControllerApi via IInterfaceManager for cross-plugin use.

using System.IO;
using Microsoft.Extensions.Logging;
using SwiftlyS2.Shared;
using SwiftlyS2.Shared.Commands;
using SwiftlyS2.Shared.Players;
using SwiftlyS2.Shared.Plugins;

using BotControllerApi;

namespace BotControllerImplSW2;

[PluginMetadata(
    Id = "botcontroller.sw2",
    Version = "1.1.5",
    Name = "BotController",
    Author = "XBribo",
    Description = "Record a player's movement and replay it on a bot."
)]
public partial class BotControllerPluginSW2(ISwiftlyCore core) : BasePlugin(core)
{
    public ILogger<BotControllerPluginSW2> Logger => Core.LoggerFactory.CreateLogger<BotControllerPluginSW2>();

    // Record and replay must share a tickrate; adjust if your server differs.
    private const int Tickrate = 64;

    private readonly ReplayDriver _driver = new();

    // ---- API implementation (cross-plugin via IInterfaceManager) ----

    private sealed class BotControllerApiImpl : IBotControllerApi
    {
        public int AbiVersion => BotController.AbiVersion;

        public bool Lock(int slot, LockKind kind) => BotController.Lock(slot, kind);
        public bool Lock(int slot, LockTarget target) => BotController.Lock(slot, target);
        public bool Unlock(int slot, LockKind kind) => BotController.Unlock(slot, kind);
        public bool UnlockAll(LockKind kind) => BotController.UnlockAll(kind);
        public bool IsLocked(int slot, LockKind kind) => BotController.IsLocked(slot, kind);
        public LockTarget GetWeaponLock(int slot) => BotController.GetWeaponLock(slot);

        public bool StartRecord(int slot) => BotController.StartRecord(slot);
        public bool StopRecord(int slot) => BotController.StopRecord(slot);
        public int RecordedTickCount(int slot) => BotController.RecordedTickCount(slot);
        public (ReplayTick[] ticks, SubtickMove[] subs) GetRecordedMotion(int slot)
            => BotController.GetRecordedMotion(slot);

        public bool LoadReplay(int slot, ReplayTick[] ticks, SubtickMove[] subs)
            => BotController.LoadReplay(slot, ticks, subs);
        public bool TransferRecordingToReplay(int srcSlot, int dstSlot)
            => BotController.TransferRecordingToReplay(srcSlot, dstSlot);
        public bool StartReplay(int slot, bool loop = false) => BotController.StartReplay(slot, loop);
        public bool StopReplay(int slot) => BotController.StopReplay(slot);
        public int ReplayCursor(int slot) => BotController.ReplayCursor(slot);
        public int ReplayTotal(int slot) => BotController.ReplayTotal(slot);
        public bool IsReplaying(int slot) => BotController.IsReplaying(slot);
        public bool TryGetReplayTick(int slot, out ReplayTick tick)
            => BotController.TryGetReplayTick(slot, out tick);

        public bool SwitchBotWeapon(int slot, int defIndex)
            => BotController.SwitchBotWeapon(slot, defIndex);
        public int BotActiveWeaponDef(int slot) => BotController.BotActiveWeaponDef(slot);

        public bool TryGetProfile(int slot, out BotProfileData profile)
            => BotController.TryGetProfile(slot, out profile);

        public bool SetBuyPlan(int slot, string aliases) => BotController.SetBuyPlan(slot, aliases);
        public bool SetBuySkip(int slot) => BotController.SetBuySkip(slot);
        public bool ClearBuyPlan(int slot) => BotController.ClearBuyPlan(slot);
        public bool ClearAllBuyPlans() => BotController.ClearAllBuyPlans();
        public int BuyPlanItemCount(int slot) => BotController.BuyPlanItemCount(slot);
    }

    // ---- Lifecycle ----

    public override void ConfigureSharedInterface(IInterfaceManager interfaceManager)
    {
        // Register the cross-plugin API so other SwiftlyS2 plugins can use it.
        // Consumers call: interfaceManager.GetSharedInterface<IBotControllerApi>("botcontroller:api")
        interfaceManager.AddSharedInterface<IBotControllerApi, BotControllerApiImpl>(
            "botcontroller:api", new BotControllerApiImpl());
    }

    public override void Load(bool hotReload)
    {
        if (!BotController.IsCompatible())
        {
            Logger.LogWarning("[BotController] BotController ABI mismatch; plugin disabled.");
            return;
        }

        Logger.LogInformation("[BotController] Loaded. hotReload={HotReload}", hotReload);

        Directory.CreateDirectory(RecordingsDir);

        // Hook the server tick for replay driver
        Core.Event.OnTick += _driver.Tick;
    }

    public override void Unload()
    {
        Core.Event.OnTick -= _driver.Tick;
        Logger.LogInformation("[BotController] Unloaded.");
    }

    // ---- Helpers ----

    private string RecordingsDir => Path.Combine(Core.PluginPath, "recordings");

    private string FileFor(ulong steamId) =>
        Path.Combine(RecordingsDir, $"{steamId}.json");

    private static string Tag(string msg) =>
        $"{Helper.ChatColors.Green}[BotController]{Helper.ChatColors.Default} {msg}".Colored();

    // ---- Chat Commands ----

    [Command("record")]
    public void OnRecord(ICommandContext context)
    {
        var player = context.Sender;
        if (player == null || !player.IsValid) return;

        if (!BotController.StartRecord(player.Slot))
        {
            context.Reply(Tag("Failed to start recording."));
            return;
        }
            context.Reply(Tag("Recording. Use !stoprecord to finish."));
    }

    [Command("stoprecord")]
    public void OnStopRecord(ICommandContext context)
    {
        var player = context.Sender;
        if (player == null || !player.IsValid) return;

        BotController.StopRecord(player.Slot);

        int saved = MotionStore.SaveToFile(player.Slot, FileFor(player.SteamID), Tickrate);
        context.Reply(saved > 0
            ? Tag($"Saved {saved} ticks.")
            : Tag("Nothing recorded."));
    }

    [Command("replay")]
    public void OnReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (player == null || !player.IsValid) return;

        if (context.Args.Length < 1 || !int.TryParse(context.Args[0], out int botSlot))
        {
            context.Reply(Tag("Usage: !replay <botSlot> [loop]"));
            return;
        }
        bool loop = context.Args.Length >= 2 && context.Args[1] == "loop";
        string file = FileFor(player.SteamID);
        if (!File.Exists(file))
        {
            context.Reply(Tag("No recording found. Use !record first."));
            return;
        }

        MotionRecording rec = MotionStore.LoadFromFile(file);
        if (rec.Ticks.Length == 0)
        {
            context.Reply(Tag("Recording is empty."));
            return;
        }
        if (rec.Tickrate != Tickrate)
            context.Reply(Tag($"WARN tickrate mismatch: recorded {rec.Tickrate}, server {Tickrate}."));

        // Freeze the bot's AI so it doesn't fight the replayed motion.
        BotController.Lock(botSlot, LockKind.All);

        if (BotController.LoadReplay(botSlot, rec.Ticks, rec.Subticks) &&
            BotController.StartReplay(botSlot, loop))
        {
            _driver.Track(botSlot);
            context.Reply(Tag($"Replaying on bot slot {botSlot}{(loop ? " (loop)" : "")}."));
        }
        else
        {
            BotController.Unlock(botSlot, LockKind.All);
            context.Reply(Tag("Failed to start replay."));
        }
    }

    [Command("stopreplay")]
    public void OnStopReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (player == null || !player.IsValid) return;

        if (context.Args.Length < 1 || !int.TryParse(context.Args[0], out int botSlot)) return;

        BotController.StopReplay(botSlot);
        _driver.Release(botSlot);
        context.Reply(Tag($"Stopped replay on bot slot {botSlot}."));
    }
}
