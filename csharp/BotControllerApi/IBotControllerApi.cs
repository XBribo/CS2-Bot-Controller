// Cross-plugin BotController API.

namespace BotControllerApi
{
    public interface IBotControllerApi
    {
        // Supported API contract version.
        int ApiVersion { get; }

        // ---- locks ----

        // All / Aim
        bool Lock(int slot, LockKind kind);

        // Locks an engine weapon slot.
        bool Lock(int slot, LockTarget target);

        bool Unlock(int slot, LockKind kind);

        bool UnlockAll(LockKind kind);

        // Queries All/Aim locks; weapon locks use GetWeaponLock.
        bool IsLocked(int slot, LockKind kind);

        // Locked weapon slot, or None.
        LockTarget GetWeaponLock(int slot);

        // ---- recording ----

        bool StartRecord(int slot);

        bool StopRecord(int slot);

        int RecordedTickCount(int slot);

        // Copies recorded ticks and subticks.
        (ReplayTick[] ticks, SubtickMove[] subs) GetRecordedMotion(int slot);

        // Copies aligned tick, subtick, and command buffers.
        (ReplayTick[] ticks, SubtickMove[] subs, ReplayCommandFrame[] commands)
            GetRecordedMotionExtended(int slot);

        // Optional input history.
        byte[] GetRecordedInputHistory(int slot);

        // ---- replay ----

        // Loads tick, subtick, and command buffers.
        bool LoadReplay(
            int slot,
            ReplayTick[] ticks,
            SubtickMove[] subs,
            ReplayCommandFrame[] commands);

        // Loads buffers with optional input history.
        bool LoadReplay(
            int slot,
            ReplayTick[] ticks,
            SubtickMove[] subs,
            ReplayCommandFrame[] commands,
            byte[]? inputHistory = null);

        // Transfers recorded buffers to a replay slot.
        bool TransferRecordingToReplay(int srcSlot, int dstSlot);

        // Registers the current native Pawn.
        bool SetReplayPawn(int slot, nint pawn);

        bool StartReplay(int slot, bool loop = false);

        bool StopReplay(int slot);

        int ReplayCursor(int slot);

        int ReplayTotal(int slot);

        bool IsReplaying(int slot);

        // Last completed frame; starts with frame 0.
        bool TryGetReplayTick(int slot, out ReplayTick tick);

        // ---- weapons ----

        // Switches a Bot/player weapon; API name retained.
        bool SwitchBotWeapon(int slot, int defIndex);

        // Bot/player weapon definition; negative if unresolved.
        int BotActiveWeaponDef(int slot);

        // Starts button injection; returns a token.
        long InjectUsercmd(int slot, ulong buttonMask, int durationMs = 0);

        // Cancels a button-injection token.
        bool CancelUsercmdInjection(int slot, long injectionId);

        // Starts persistent movement; returns a token.
        long StartUsercmdMovement(int slot, float forwardMove, float leftMove);

        // Updates a movement token.
        bool UpdateUsercmdMovement(
            int slot,
            long movementId,
            float forwardMove,
            float leftMove);

        // Cancels a movement token.
        bool CancelUsercmdMovement(int slot, long movementId);

        // Suppresses buttons for a fixed duration.
        bool SuppressUsercmd(int slot, ulong buttonMask, int durationMs);

        // Starts persistent suppression; returns a token.
        long StartUsercmdSuppression(int slot, ulong buttonMask);

        // Cancels a suppression token.
        bool CancelUsercmdSuppression(int slot, long suppressionId);

        // ---- profile ----

        // Reads the current Bot profile; false if unavailable.
        bool GetBotProfile(int slot, out BotProfileData profile);

        // ---- buy plans ----

        // Force a bot's per-round buy.
        bool SetBuyPlan(int slot, string aliases);

        // Force a bot to buy nothing each round.
        bool SetBuySkip(int slot);

        // Clears the plan and restores native buying.
        bool ClearBuyPlan(int slot);

        bool ClearAllBuyPlans();

        // Buy count: -1 no plan, 0 skip/empty, >0 alias count.
        int BuyPlanItemCount(int slot);

        // ---- voice ----

        // Checks voice availability.
        bool CanSendVoice();

        // Returns 0 when ready; negative values report setup errors.
        int GetVoiceStatus();

        // Sends an Opus frame to the recipient slot.
        int SendVoiceFrame(
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
            int audibleMask);
    }
}
