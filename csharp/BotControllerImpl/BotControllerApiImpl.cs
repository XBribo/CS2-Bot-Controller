// Provider-side implementation of IBotControllerApi.

namespace BotControllerApi
{
    public sealed class BotControllerApiImpl : IBotControllerApi
    {
        public int AbiVersion => BotController.AbiVersion;

        // ---- locks ----
        public bool Lock(int slot, LockKind kind) => BotController.Lock(slot, kind);
        public bool Lock(int slot, LockTarget target) => BotController.Lock(slot, target);
        public bool Unlock(int slot, LockKind kind) => BotController.Unlock(slot, kind);
        public bool UnlockAll(LockKind kind) => BotController.UnlockAll(kind);
        public bool IsLocked(int slot, LockKind kind) => BotController.IsLocked(slot, kind);
        public LockTarget GetWeaponLock(int slot) => BotController.GetWeaponLock(slot);

        // ---- recording ----
        public bool StartRecord(int slot) => BotController.StartRecord(slot);
        public bool StopRecord(int slot) => BotController.StopRecord(slot);
        public int RecordedTickCount(int slot) => BotController.RecordedTickCount(slot);
        // Returns recording frames using the caller's captured tickrate.
        public ReplayData GetRecordedMotion(int slot, float tickRate)
            => BotController.GetRecordedMotion(slot, tickRate);

        // ---- replay ----
        // Copies one unified frame sequence into the native replay buffer.
        public bool LoadReplay(int slot, ReplayData replay) => BotController.LoadReplay(slot, replay);
        public bool TransferRecordingToReplay(int srcSlot, int dstSlot)
            => BotController.TransferRecordingToReplay(srcSlot, dstSlot);
        // Registers the authoritative native pawn pointer for replay.
        public bool SetReplayPawn(int slot, nint pawn) => BotController.SetReplayPawn(slot, pawn);
        public bool StartReplay(int slot, bool loop = false) => BotController.StartReplay(slot, loop);
        // Starts or resumes replay at an inclusive index.
        public bool StartReplayAt(int slot, bool loop, int startIndex)
            => BotController.StartReplayAt(slot, loop, startIndex);
        // Holds input before the exclusive boundary while retaining replay ownership.
        public bool StartReplayUntil(int slot, bool loop, int startIndex, int holdBeforeIndex)
            => BotController.StartReplayUntil(slot, loop, startIndex, holdBeforeIndex);
        public bool StopReplay(int slot) => BotController.StopReplay(slot);
        // Stops replay and releases its buffer allocations.
        public bool ReleaseReplayBuffer(int slot) => BotController.ReleaseReplayBuffer(slot);
        public int ReplayCursor(int slot) => BotController.ReplayCursor(slot);
        public int ReplayTotal(int slot) => BotController.ReplayTotal(slot);
        // Reads aggregate replay state without losing the terminal cursor.
        public bool TryGetReplayState(int slot, out ReplaySlotState state) => BotController.TryGetReplayState(slot, out state);
        public bool IsReplaying(int slot) => BotController.IsReplaying(slot);
        // Returns snapshots, input and subticks from the same native frame.
        public bool TryGetReplayFrame(int slot, out ReplayFrame frame)
            => BotController.TryGetReplayFrame(slot, out frame);

        // ---- weapons ----
        public bool SwitchBotWeapon(int slot, int defIndex)
            => BotController.SwitchBotWeapon(slot, defIndex);
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

        // ---- profile ----
        public bool GetBotProfile(int slot, out BotProfileData profile)
            => BotController.GetBotProfile(slot, out profile);

        // ---- buy plans ----
        public bool SetBuyPlan(int slot, string aliases) => BotController.SetBuyPlan(slot, aliases);
        public bool SetBuySkip(int slot) => BotController.SetBuySkip(slot);
        public bool ClearBuyPlan(int slot) => BotController.ClearBuyPlan(slot);
        public bool ClearAllBuyPlans() => BotController.ClearAllBuyPlans();
        public int BuyPlanItemCount(int slot) => BotController.BuyPlanItemCount(slot);

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
