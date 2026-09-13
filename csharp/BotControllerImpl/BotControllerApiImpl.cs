// Provider-side implementation of IBotControllerApi.

using BotControllerImpl;

namespace BotControllerApi
{
    public sealed class BotControllerApiImpl : IBotControllerApi
    {
        private readonly BotControllerPlugin _plugin;

        public BotControllerApiImpl(BotControllerPlugin plugin)
        {
            _plugin = plugin;
        }

        private bool CanControlBot(int slot) =>
            _plugin.RuntimeEnabled &&
            BotControllerPlugin.TryGetLiveBot(slot, out _);

        public int ApiVersion => 2;

        public int AbiVersion => BotController.AbiVersion;

        public bool RuntimePrepared => BotController.RuntimePrepared;

        public bool RuntimeEnabled => _plugin.RuntimeEnabled;

        public bool SetRuntimeEnabled(bool enabled) => _plugin.SetRuntimeEnabled(enabled);

        public bool IsLiveBotSlot(int slot) => BotController.IsLiveBotSlot(slot);

        // ---- locks ----
        public bool Lock(int slot, LockKind kind)
            => CanControlBot(slot) && BotController.Lock(slot, kind);
        public bool Lock(int slot, LockTarget target)
            => CanControlBot(slot) && BotController.Lock(slot, target);
        public bool Unlock(int slot, LockKind kind)
            => CanControlBot(slot) && BotController.Unlock(slot, kind);
        public bool UnlockAll(LockKind kind) => BotController.UnlockAll(kind);
        public bool IsLocked(int slot, LockKind kind)
            => CanControlBot(slot) && BotController.IsLocked(slot, kind);
        public LockTarget GetWeaponLock(int slot)
            => CanControlBot(slot) ? BotController.GetWeaponLock(slot) : LockTarget.None;

        // ---- recording ----
        public bool StartRecord(int slot)
            => _plugin.RuntimeEnabled && BotController.StartRecord(slot);
        public bool StopRecord(int slot) => BotController.StopRecord(slot);
        public int RecordedTickCount(int slot) => BotController.RecordedTickCount(slot);
        public (ReplayTick[] ticks, SubtickMove[] subs) GetRecordedMotion(int slot)
            => BotController.GetRecordedMotion(slot);
        // Returns aligned tick, subtick, and command-frame buffers
        public (ReplayTick[] ticks, SubtickMove[] subs, ReplayCommandFrame[] commands)
            GetRecordedMotionExtended(int slot)
            => BotController.GetRecordedMotionExtended(slot);

        // ---- replay ----
        public bool LoadReplay(int slot, ReplayTick[] ticks, SubtickMove[] subs)
            => CanControlBot(slot) && BotController.LoadReplay(slot, ticks, subs);
        // Loads aligned command frames without movement-extra data
        public bool LoadReplayExtended(
            int slot,
            ReplayTick[] ticks,
            SubtickMove[] subs,
            ReplayCommandFrame[] commands)
            => CanControlBot(slot) && BotController.LoadReplayExtended(
                slot, ticks, subs, commands, Array.Empty<ReplayMovementExtra>());
        public bool TransferRecordingToReplay(int srcSlot, int dstSlot)
            => CanControlBot(dstSlot) && BotController.TransferRecordingToReplay(srcSlot, dstSlot);
        // Registers the authoritative native pawn pointer for replay.
        public bool SetReplayPawn(int slot, nint pawn)
            => CanControlBot(slot) && BotController.SetReplayPawn(slot, pawn);
        public bool StartReplay(int slot, bool loop = false)
            => CanControlBot(slot) && BotController.StartReplay(slot, loop);
        public bool StopReplay(int slot) => BotController.StopReplay(slot);
        public int ReplayCursor(int slot)
            => CanControlBot(slot) ? BotController.ReplayCursor(slot) : -1;
        public int ReplayTotal(int slot)
            => CanControlBot(slot) ? BotController.ReplayTotal(slot) : 0;
        public bool IsReplaying(int slot)
            => CanControlBot(slot) && BotController.IsReplaying(slot);
        public bool TryGetReplayTick(int slot, out ReplayTick tick)
        {
            if (CanControlBot(slot))
                return BotController.TryGetReplayTick(slot, out tick);

            tick = default;
            return false;
        }

        // ---- weapons ----
        public bool SwitchBotWeapon(int slot, int defIndex)
            => CanControlBot(slot) && BotController.SwitchBotWeapon(slot, defIndex);
        public int BotActiveWeaponDef(int slot)
            => CanControlBot(slot) ? BotController.BotActiveWeaponDef(slot) : -1;
        // Creates an independently cancellable native usercmd injection
        public long InjectUsercmd(int slot, ulong buttonMask, int durationMs = 0)
            => CanControlBot(slot) ? BotController.InjectUsercmd(slot, buttonMask, durationMs) : -1;
        // Cancels one native usercmd injection by its token
        public bool CancelUsercmdInjection(int slot, long injectionId)
            => CanControlBot(slot) && BotController.CancelUsercmdInjection(slot, injectionId);
        // Creates an independently cancellable persistent analog movement override
        public long StartUsercmdMovement(int slot, float forwardMove, float leftMove)
            => CanControlBot(slot)
                ? BotController.StartUsercmdMovement(slot, forwardMove, leftMove)
                : -1;
        // Updates one persistent analog movement override
        public bool UpdateUsercmdMovement(
            int slot,
            long movementId,
            float forwardMove,
            float leftMove)
            => CanControlBot(slot) && BotController.UpdateUsercmdMovement(
                slot, movementId, forwardMove, leftMove);
        // Cancels one persistent analog movement override
        public bool CancelUsercmdMovement(int slot, long movementId)
            => CanControlBot(slot) && BotController.CancelUsercmdMovement(slot, movementId);
        // Suppresses selected usercmd buttons for a fixed duration
        public bool SuppressUsercmd(int slot, ulong buttonMask, int durationMs)
            => CanControlBot(slot) && BotController.SuppressUsercmd(slot, buttonMask, durationMs);
        // Creates an independently cancellable persistent native usercmd suppression
        public long StartUsercmdSuppression(int slot, ulong buttonMask)
            => CanControlBot(slot) ? BotController.StartUsercmdSuppression(slot, buttonMask) : -1;
        // Cancels one persistent native usercmd suppression by its token
        public bool CancelUsercmdSuppression(int slot, long suppressionId)
            => CanControlBot(slot) && BotController.CancelUsercmdSuppression(slot, suppressionId);

        // ---- profile ----
        public bool GetBotProfile(int slot, out BotProfileData profile)
        {
            if (CanControlBot(slot))
                return BotController.GetBotProfile(slot, out profile);

            profile = default;
            return false;
        }

        // ---- buy plans ----
        public bool SetBuyPlan(int slot, string aliases)
            => CanControlBot(slot) && BotController.SetBuyPlan(slot, aliases);
        public bool SetBuySkip(int slot)
            => CanControlBot(slot) && BotController.SetBuySkip(slot);
        public bool ClearBuyPlan(int slot)
            => CanControlBot(slot) && BotController.ClearBuyPlan(slot);
        public bool ClearAllBuyPlans() => BotController.ClearAllBuyPlans();
        public int BuyPlanItemCount(int slot)
            => CanControlBot(slot) ? BotController.BuyPlanItemCount(slot) : -1;

        // ---- voice ----
        public bool CanSendVoice() => BotController.CanSendVoice();
        public int GetVoiceStatus() => BotController.GetVoiceStatus();
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
}
