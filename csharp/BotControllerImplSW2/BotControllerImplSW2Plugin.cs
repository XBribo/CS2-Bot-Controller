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

    private readonly ReplayDriver _driver = new();
    private readonly Dictionary<int, string> _recordingFiles = new();
    private readonly HashSet<int> _savingSlots = new();
    private readonly Dictionary<int, object> _loadingSlots = new();
    private readonly HashSet<int> _cancelledLoads = new();
    private readonly ConcurrentQueue<Action> _completedJobs = new();
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
        // Returns aligned tick, subtick, and command-frame buffers for a slot
        public (ReplayTick[] ticks, SubtickMove[] subs, ReplayCommandFrame[] commands)
            GetRecordedMotionExtended(int slot)
            => BotController.GetRecordedMotionExtended(slot);

        // Loads a replay buffer into a bot slot.
        public bool LoadReplay(
            int slot,
            ReplayTick[] ticks,
            SubtickMove[] subs,
            ReplayCommandFrame[] commands)
            => BotController.LoadReplay(
                slot, ticks, subs, commands, Array.Empty<ReplayMovementExtra>());
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
        // Creates an independently cancellable native usercmd injection
        public long InjectUsercmd(int slot, ulong buttonMask, int durationMs = 0)
            => BotController.InjectUsercmd(slot, buttonMask, durationMs);
        // Cancels one native usercmd injection by its token
        public bool CancelUsercmdInjection(int slot, long injectionId)
            => BotController.CancelUsercmdInjection(slot, injectionId);
        // Creates an independently cancellable persistent analog movement override
        public long StartUsercmdMovement(int slot, float forwardMove, float leftMove)
            => BotController.StartUsercmdMovement(slot, forwardMove, leftMove);
        // Updates one persistent analog movement override
        public bool UpdateUsercmdMovement(
            int slot,
            long movementId,
            float forwardMove,
            float leftMove)
            => BotController.UpdateUsercmdMovement(
                slot, movementId, forwardMove, leftMove);
        // Cancels one persistent analog movement override
        public bool CancelUsercmdMovement(int slot, long movementId)
            => BotController.CancelUsercmdMovement(slot, movementId);
        // Suppresses selected usercmd buttons for a fixed duration
        public bool SuppressUsercmd(int slot, ulong buttonMask, int durationMs)
            => BotController.SuppressUsercmd(slot, buttonMask, durationMs);
        // Creates an independently cancellable persistent native usercmd suppression
        public long StartUsercmdSuppression(int slot, ulong buttonMask)
            => BotController.StartUsercmdSuppression(slot, buttonMask);
        // Cancels one persistent native usercmd suppression by its token
        public bool CancelUsercmdSuppression(int slot, long suppressionId)
            => BotController.CancelUsercmdSuppression(slot, suppressionId);

        // Returns the live profile data for a bot slot.
        public bool GetBotProfile(int slot, out BotProfileData profile)
            => BotController.GetBotProfile(slot, out profile);

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
    public override void Load(bool _)
    {
        Logger.LogInformation("{Version}, built {BuildTime}", PluginBuildInfo.DisplayVersion, PluginBuildInfo.BuildTime);
        if (!EnsureNativeApiAvailability()) return;

        Directory.CreateDirectory(RecordingsDir);

        // Hook the server tick for replay driver
        Core.Event.OnTick += OnTick;
    }

    // Unhooks replay ticking during plugin unload.
    public override void Unload()
    {
        if (_nativeApiAvailable)
        {
            Core.Event.OnTick -= OnTick;
        }
    }

    // ---- Helpers ----

    // Applies completed file jobs on the game thread before replay bookkeeping.
    private void OnTick()
    {
        while (_completedJobs.TryDequeue(out Action? completion)) completion();
        _driver.Tick();
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

    // Registers the live pawn before the first recording or replay frame.
    private bool RegisterPawnForSlot(int slot)
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
                "[BotController] BotController ABI mismatch; got ABI {AbiVersion}.",
                BotController.AbiVersion);
            return false;
        }
        catch (Exception ex)
        {
            Logger.LogWarning(ex, "[BotController] Failed to initialize BotController API.");
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
        if (!RegisterPawnForSlot(player.Slot) || !BotController.StartRecord(player.Slot))
        {
            context.Reply(Tag("Failed to start recording."));
            return;
        }
        _recordingFiles[player.Slot] = file;
        context.Reply(Tag("Recording. Use !stoprecord to finish."));
    }

    // Stops recording and persists the invoking player's captured motion.
    [Command("stoprecord")]
    public void OnStopRecord(ICommandContext context)
    {
        var player = context.Sender;
        if (player == null || !player.IsValid) return;
        if (_savingSlots.Contains(player.Slot))
        {
            context.Reply(Tag("Recording is still saving."));
            return;
        }

        BotController.StopRecord(player.Slot);

        if (!_recordingFiles.Remove(player.Slot, out string? file) &&
            !TryGetRecordingFile(null, player.SteamID, out file))
            return;

        int slot = player.Slot;
        ulong steamId = player.SteamID;
        _savingSlots.Add(slot);
        context.Reply(Tag("Saving recording..."));
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
                    Logger.LogError(ex, "Recording save failed.");
                    NotifyPlayer(slot, steamId, "Recording save failed.");
                });
            }
        });
    }

    // Loads the invoking player's recording and starts replay on a target bot slot.
    [Command("replay")]
    public void OnReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (player == null || !player.IsValid) return;

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
        int requesterSlot = player.Slot;
        ulong requesterSteamId = player.SteamID;
        object token = new();
        _loadingSlots.Add(botSlot, token);
        context.Reply(Tag("Loading replay..."));
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
                    Logger.LogError(ex, "Replay load failed.");
                    NotifyPlayer(requesterSlot, requesterSteamId, "Replay load failed.");
                });
            }
        });
    }

    // Stops replay on a target bot slot and releases the bot lock.
    [Command("stopreplay")]
    public void OnStopReplay(ICommandContext context)
    {
        var player = context.Sender;
        if (player == null || !player.IsValid) return;

        if (context.Args.Length < 1 || !int.TryParse(context.Args[0], out int botSlot)) return;

        if (_loadingSlots.ContainsKey(botSlot)) _cancelledLoads.Add(botSlot);

        BotController.StopReplay(botSlot);
        _driver.Release(botSlot);
        context.Reply(Tag($"Stopped replay on bot slot {botSlot}."));
    }
}
