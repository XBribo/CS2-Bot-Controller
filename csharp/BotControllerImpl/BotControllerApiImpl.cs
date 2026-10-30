// Shared provider-side implementation of IBotControllerApi.

namespace BotControllerApi
{
    public sealed class BotControllerApiImpl : IBotControllerApi, IDisposable
    {
        private volatile bool _active = true;

        public int ApiVersion => 1;

        public void Dispose() => _active = false;

        // ---- locks ----
        public bool Lock(int slot, LockKind kind) => _active && BotController.Lock(slot, kind);
        public bool Lock(int slot, LockTarget target) => _active && BotController.Lock(slot, target);
        public bool Unlock(int slot, LockKind kind) => _active && BotController.Unlock(slot, kind);
        public bool UnlockAll(LockKind kind) => _active && BotController.UnlockAll(kind);
        public bool IsLocked(int slot, LockKind kind) => _active && BotController.IsLocked(slot, kind);
        public LockTarget GetWeaponLock(int slot) => _active ? BotController.GetWeaponLock(slot) : LockTarget.None;

        // ---- recording ----
        public bool StartRecord(int slot) => _active && BotController.StartRecord(slot);
        public bool StopRecord(int slot) => _active && BotController.StopRecord(slot);
        public int RecordedTickCount(int slot) => _active ? BotController.RecordedTickCount(slot) : 0;
        public (ReplayTick[] ticks, SubtickMove[] subs) GetRecordedMotion(int slot)
            => _active ? BotController.GetRecordedMotion(slot) : ([], []);
        // Copies aligned tick, subtick, and command buffers.
        public (ReplayTick[] ticks, SubtickMove[] subs, ReplayCommandFrame[] commands)
            GetRecordedMotionExtended(int slot)
            => _active ? BotController.GetRecordedMotionExtended(slot) : ([], [], []);
        // Copies optional input history.
        public byte[] GetRecordedInputHistory(int slot) => _active ? BotController.GetRecordedInputHistory(slot) : [];

        // ---- replay ----
        public bool LoadReplay(
            int slot,
            ReplayTick[] ticks,
            SubtickMove[] subs,
            ReplayCommandFrame[] commands)
            => _active && BotController.LoadReplay(slot, ticks, subs, commands);

        public bool LoadReplay(
            int slot,
            ReplayTick[] ticks,
            SubtickMove[] subs,
            ReplayCommandFrame[] commands,
            byte[]? inputHistory = null)
            => _active && BotController.LoadReplay(slot, ticks, subs, commands, inputHistory);
        public bool TransferRecordingToReplay(int srcSlot, int dstSlot)
            => _active && BotController.TransferRecordingToReplay(srcSlot, dstSlot);
        // Registers the current native Pawn.
        public bool SetReplayPawn(int slot, nint pawn) => _active && BotController.SetReplayPawn(slot, pawn);
        public bool StartReplay(int slot, bool loop = false) => _active && BotController.StartReplay(slot, loop);
        public bool StopReplay(int slot) => _active && BotController.StopReplay(slot);
        public int ReplayCursor(int slot) => _active ? BotController.ReplayCursor(slot) : -1;
        public int ReplayTotal(int slot) => _active ? BotController.ReplayTotal(slot) : 0;
        public bool IsReplaying(int slot) => _active && BotController.IsReplaying(slot);
        public bool TryGetReplayTick(int slot, out ReplayTick tick)
        {
            tick = default;
            return _active && BotController.TryGetReplayTick(slot, out tick);
        }

        // ---- weapons ----
        public bool SwitchBotWeapon(int slot, int defIndex)
            => _active && BotController.SwitchBotWeapon(slot, defIndex);
        public int BotActiveWeaponDef(int slot) => _active ? BotController.BotActiveWeaponDef(slot) : -1;
        // Starts button injection; returns a token.
        public long InjectUsercmd(int slot, ulong buttonMask, int durationMs = 0)
            => _active ? BotController.InjectUsercmd(slot, buttonMask, durationMs) : 0;
        // Cancels a button-injection token.
        public bool CancelUsercmdInjection(int slot, long injectionId)
            => _active && BotController.CancelUsercmdInjection(slot, injectionId);
        // Starts persistent movement; returns a token.
        public long StartUsercmdMovement(int slot, float forwardMove, float leftMove)
            => _active ? BotController.StartUsercmdMovement(slot, forwardMove, leftMove) : 0;
        // Updates a movement token.
        public bool UpdateUsercmdMovement(
            int slot,
            long movementId,
            float forwardMove,
            float leftMove)
            => _active && BotController.UpdateUsercmdMovement(
                slot, movementId, forwardMove, leftMove);
        // Cancels a movement token.
        public bool CancelUsercmdMovement(int slot, long movementId)
            => _active && BotController.CancelUsercmdMovement(slot, movementId);
        // Suppresses buttons for a fixed duration.
        public bool SuppressUsercmd(int slot, ulong buttonMask, int durationMs)
            => _active && BotController.SuppressUsercmd(slot, buttonMask, durationMs);
        // Starts persistent suppression; returns a token.
        public long StartUsercmdSuppression(int slot, ulong buttonMask)
            => _active ? BotController.StartUsercmdSuppression(slot, buttonMask) : 0;
        // Cancels a suppression token.
        public bool CancelUsercmdSuppression(int slot, long suppressionId)
            => _active && BotController.CancelUsercmdSuppression(slot, suppressionId);

        // ---- profile ----
        public bool GetBotProfile(int slot, out BotProfileData profile)
        {
            profile = default;
            return _active && BotController.GetBotProfile(slot, out profile);
        }

        // ---- buy plans ----
        public bool SetBuyPlan(int slot, string aliases) => _active && BotController.SetBuyPlan(slot, aliases);
        public bool SetBuySkip(int slot) => _active && BotController.SetBuySkip(slot);
        public bool ClearBuyPlan(int slot) => _active && BotController.ClearBuyPlan(slot);
        public bool ClearAllBuyPlans() => _active && BotController.ClearAllBuyPlans();
        public int BuyPlanItemCount(int slot) => _active ? BotController.BuyPlanItemCount(slot) : -1;

        // ---- voice ----
        public bool CanSendVoice() => _active && BotController.CanSendVoice();
        public int GetVoiceStatus() => _active ? BotController.GetVoiceStatus() : -1;
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
            => _active ? BotController.SendVoiceFrame(
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
                audibleMask) : -1;
    }
}
