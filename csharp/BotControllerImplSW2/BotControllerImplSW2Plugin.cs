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
    Version = "0.5.1",
    Name = "BotController",
    Author = "XBribo & nicedayzhu",
    Description = "Record a player's movement and replay it on a bot."
)]
public partial class BotControllerImplSW2Plugin(ISwiftlyCore core) : BasePlugin(core)
{
    public ILogger<BotControllerImplSW2Plugin> Logger => Core.LoggerFactory.CreateLogger<BotControllerImplSW2Plugin>();

    // Record and replay must share a tickrate; adjust if your server differs.
    private const int Tickrate = 64;
    private const string InterfaceKey = "botcontroller:api";

    private readonly ReplayDriver _driver = new();
    private bool _nativeApiChecked;
    private bool _nativeApiAvailable;

    // ---- API implementation (cross-plugin via IInterfaceManager) ----

    private sealed class BotControllerApiImpl : IBotControllerApi
    {
        // Returns the active native ABI version.
        public int AbiVersion => BotController.AbiVersion;

        // Applies a generic lock to a bot slot.
        public bool Lock(int slot, LockKind kind) => BotController.Lock(slot, kind);
        // Applies a weapon-slot lock to a bot slot.
        public bool Lock(int slot, LockTarget target) => BotController.Lock(slot, target);
        // Releases a specific lock from a bot slot.
        public bool Unlock(int slot, LockKind kind) => BotController.Unlock(slot, kind);
        // Releases the same lock kind from every tracked bot slot.
        public bool UnlockAll(LockKind kind) => BotController.UnlockAll(kind);
        // Reports whether a specific lock is active for a slot.
        public bool IsLocked(int slot, LockKind kind) => BotController.IsLocked(slot, kind);
        // Returns the active weapon-lock target for a slot.
        public LockTarget GetWeaponLock(int slot) => BotController.GetWeaponLock(slot);

        // Starts recording movement for a slot.
        public bool StartRecord(int slot) => BotController.StartRecord(slot);
        // Stops recording movement for a slot.
        public bool StopRecord(int slot) => BotController.StopRecord(slot);
        // Returns the number of recorded ticks for a slot.
        public int RecordedTickCount(int slot) => BotController.RecordedTickCount(slot);
        // Returns the recorded tick and subtick buffers for a slot.
        public (ReplayTick[] ticks, SubtickMove[] subs) GetRecordedMotion(int slot)
            => BotController.GetRecordedMotion(slot);

        // Loads a replay buffer into a bot slot.
        public bool LoadReplay(int slot, ReplayTick[] ticks, SubtickMove[] subs)
            => BotController.LoadReplay(slot, ticks, subs);
        // Moves a recorded buffer directly into another slot's replay buffer.
        public bool TransferRecordingToReplay(int srcSlot, int dstSlot)
            => BotController.TransferRecordingToReplay(srcSlot, dstSlot);
        // Registers the authoritative native pawn pointer for replay.
        public bool SetReplayPawn(int slot, nint pawn) => BotController.SetReplayPawn(slot, pawn);
        // Starts replay for a bot slot.
        public bool StartReplay(int slot, bool loop = false) => BotController.StartReplay(slot, loop);
        // Stops replay for a bot slot.
        public bool StopReplay(int slot) => BotController.StopReplay(slot);
        // Returns the current replay cursor for a slot.
        public int ReplayCursor(int slot) => BotController.ReplayCursor(slot);
        // Returns the total replay length for a slot.
        public int ReplayTotal(int slot) => BotController.ReplayTotal(slot);
        // Reports whether a slot is actively replaying.
        public bool IsReplaying(int slot) => BotController.IsReplaying(slot);
        // Returns the current replay tick when one is available.
        public bool TryGetReplayTick(int slot, out ReplayTick tick)
            => BotController.TryGetReplayTick(slot, out tick);

        // Switches the active weapon for a bot slot.
        public bool SwitchBotWeapon(int slot, int defIndex)
            => BotController.SwitchBotWeapon(slot, defIndex);
        // Returns the active weapon definition index for a bot slot.
        public int BotActiveWeaponDef(int slot) => BotController.BotActiveWeaponDef(slot);

        // Returns the live profile data for a bot slot.
        public bool TryGetProfile(int slot, out BotProfileData profile)
            => BotController.TryGetProfile(slot, out profile);

        // Sets the round buy plan for a bot slot.
        public bool SetBuyPlan(int slot, string aliases) => BotController.SetBuyPlan(slot, aliases);
        // Forces a bot slot to skip buying.
        public bool SetBuySkip(int slot) => BotController.SetBuySkip(slot);
        // Clears the round buy plan for a bot slot.
        public bool ClearBuyPlan(int slot) => BotController.ClearBuyPlan(slot);
        // Clears every active round buy plan.
        public bool ClearAllBuyPlans() => BotController.ClearAllBuyPlans();
        // Returns the number of configured buy-plan items.
        public int BuyPlanItemCount(int slot) => BotController.BuyPlanItemCount(slot);

        // Reports whether native voice frame sending is available.
        public bool CanSendVoice() => BotController.CanSendVoice();
        // Returns the native voice sender setup status.
        public int GetVoiceStatus() => BotController.GetVoiceStatus();
        // Sends one encoded Opus voice frame through the native module.
        public int SendVoiceFrame(
            int recipientSlot,
            int senderClient,
            ulong senderXuid,
            byte[] audio,
            int audioBytes,
            int sampleRate,
            float voiceLevel,
            int sequenceBytes,
            int sectionNumber,
            int uncompressedSampleOffset,
            uint numPackets,
            uint[] packetOffsets,
            int packetOffsetCount,
            int tick,
            int audibleMask)
            => BotController.SendVoiceFrame(
                recipientSlot,
                senderClient,
                senderXuid,
                audio,
                audioBytes,
                sampleRate,
                voiceLevel,
                sequenceBytes,
                sectionNumber,
                uncompressedSampleOffset,
                numPackets,
                packetOffsets,
                packetOffsetCount,
                tick,
                audibleMask);
    }

    // ---- Lifecycle ----

    // Registers the shared API only when the native BotController bridge is usable.
    public override void ConfigureSharedInterface(IInterfaceManager interfaceManager)
    {
        if (!EnsureNativeApiAvailability()) return;

        // Consumers call: interfaceManager.TryGetSharedInterface<IBotControllerApi>("botcontroller:api", out var api)
        interfaceManager.AddSharedInterface<IBotControllerApi, BotControllerApiImpl>(
            InterfaceKey, new BotControllerApiImpl());
    }

    // Loads the plugin and hooks replay ticking when the native ABI is ready.
    public override void Load(bool hotReload)
    {
        if (!EnsureNativeApiAvailability()) return;

        Logger.LogInformation("[BotController] Loaded. hotReload={HotReload}", hotReload);

        Directory.CreateDirectory(RecordingsDir);

        // Hook the server tick for replay driver
        Core.Event.OnTick += _driver.Tick;
    }

    // Unhooks replay ticking during plugin unload.
    public override void Unload()
    {
        if (_nativeApiAvailable)
            Core.Event.OnTick -= _driver.Tick;
        Logger.LogInformation("[BotController] Unloaded.");
    }

    // ---- Helpers ----

    // Returns the plugin-local recordings directory.
    private string RecordingsDir => Path.Combine(Core.PluginPath, "recordings");

    // Returns the recording path for a player's SteamID.
    private string FileFor(ulong steamId) =>
        Path.Combine(RecordingsDir, $"{steamId}.json");

    // Formats a colored chat tag for BotController replies.
    private static string Tag(string msg) =>
        $"{Helper.ChatColors.Green}[BotController]{Helper.ChatColors.Default} {msg}".Colored();

    // Registers the live bot pawn pointer required by the current native replay path.
    private bool RegisterReplayPawnForSlot(int slot)
    {
        var player = Core.PlayerManager.GetPlayer(slot);
        var pawn = player?.PlayerPawn;
        return pawn is { IsValid: true } &&
               BotController.SetReplayPawn(
                   slot,
                   ((SwiftlyS2.Shared.Natives.INativeHandle)pawn).Address);
    }

    // Returns the cached native availability state, probing once on first use.
    private bool EnsureNativeApiAvailability()
    {
        if (_nativeApiChecked) return _nativeApiAvailable;

        _nativeApiChecked = true;
        _nativeApiAvailable = DetectNativeApiAvailability();
        return _nativeApiAvailable;
    }

    // Probes whether the native bridge can be called safely and matches the expected ABI.
    private bool DetectNativeApiAvailability()
    {
        try
        {
            if (BotController.IsCompatible())
                return true;

            Logger.LogWarning(
                "[BotController] BotController ABI mismatch; expected a compatible native module, got ABI {AbiVersion}.",
                BotController.AbiVersion);
            return false;
        }
        catch (Exception ex)
        {
            Logger.LogWarning(ex, "[BotController] Failed to initialize native BotController API.");
            return false;
        }
    }

    // ---- Chat Commands ----

    // Starts recording the invoking player's movement.
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

    // Stops recording and persists the invoking player's captured motion.
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

    // Loads the invoking player's recording and starts replay on a target bot slot.
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
            RegisterReplayPawnForSlot(botSlot) &&
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

    // Stops replay on a target bot slot and releases the bot lock.
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
