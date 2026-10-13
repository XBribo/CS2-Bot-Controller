// Cross-plugin BotController API.

namespace BotControllerApi
{
    public interface IBotControllerApi
    {
        int AbiVersion { get; }

        // ---- locks ----

        // All / Aim
        bool Lock(int slot, LockKind kind);

        // Weapon: lock the bot onto a specific engine weapon slot.
        bool Lock(int slot, LockTarget target);

        bool Unlock(int slot, LockKind kind);

        bool UnlockAll(LockKind kind);

        // For All/Aim returns true if locked; for Weapon use GetWeaponLock.
        bool IsLocked(int slot, LockKind kind);

        // Weapon-only query: the locked weapon slot, or None.
        LockTarget GetWeaponLock(int slot);

        // ---- recording ----

        bool StartRecord(int slot);

        bool StopRecord(int slot);

        int RecordedTickCount(int slot);

        // Pull a stopped recording into the same model used by replay producers.
        ReplayData GetRecordedMotion(int slot, float tickRate);

        // ---- replay ----

        // Copies one complete replay; nullable fields preserve presence independently of value.
        bool LoadReplay(int slot, ReplayData replay);

        // Move a slot's just-recorded buffers into another slot's replay buffer.
        bool TransferRecordingToReplay(int srcSlot, int dstSlot);

        // Registers the current native pawn pointer before replay starts.
        bool SetReplayPawn(int slot, nint pawn);

        bool StartReplay(int slot, bool loop = false);

        // Starts at an inclusive index, or resumes a hold at that index without reinitialization.
        bool StartReplayAt(int slot, bool loop, int startIndex);

        // Plays [startIndex, holdBeforeIndex), then retains replay ownership without consuming input.
        bool StartReplayUntil(int slot, bool loop, int startIndex, int holdBeforeIndex);

        bool StopReplay(int slot);

        // Stops replay and frees all replay buffer allocations; recordings remain available.
        bool ReleaseReplayBuffer(int slot);

        int ReplayCursor(int slot);

        int ReplayTotal(int slot);

        // Reads aggregate state, including an idle slot's terminal cursor.
        bool TryGetReplayState(int slot, out ReplaySlotState state);

        bool IsReplaying(int slot);

        // The complete frame currently being replayed, including input and subticks.
        bool TryGetReplayFrame(int slot, out ReplayFrame frame);

        // ---- weapons ----

        // Switch a bot to the weapon with this def index.
        bool SwitchBotWeapon(int slot, int defIndex);

        // Queues one native AI weapon choice after Update; true means accepted, not already equipped.
        bool RequestEquipBestWeapon(int slot);

        // Reads native AI evidence only for the current bot/pawn incarnation.
        bool TryGetNativePerceptionState(int slot, out BotPerceptionState state);

        // Disables only the FOV cone during replay; native LOS and smoke checks remain authoritative.
        bool SetReplayNativeFovOverride(bool enabled);

        // Def index of the bot's current active weapon. <0 if unresolved.
        int BotActiveWeaponDef(int slot);

        // Creates an independently cancellable usercmd injection and returns its token
        long InjectUsercmd(int slot, ulong buttonMask, int durationMs = 0);

        // Cancels one usercmd injection by its token
        bool CancelUsercmdInjection(int slot, long injectionId);

        // Creates an independently cancellable persistent analog movement override
        long StartUsercmdMovement(int slot, float forwardMove, float leftMove);

        // Updates one persistent analog movement override
        bool UpdateUsercmdMovement(
            int slot,
            long movementId,
            float forwardMove,
            float leftMove);

        // Cancels one persistent analog movement override
        bool CancelUsercmdMovement(int slot, long movementId);

        // Suppresses selected usercmd buttons for a fixed duration
        bool SuppressUsercmd(int slot, ulong buttonMask, int durationMs);

        // Creates an independently cancellable persistent usercmd suppression
        long StartUsercmdSuppression(int slot, ulong buttonMask);

        // Cancels one persistent usercmd suppression by its token
        bool CancelUsercmdSuppression(int slot, long suppressionId);

        // ---- profile ----

        // Read the BotProfile of the bot on this slot. False if the slot has no
        // live bot or a null profile.
        bool GetBotProfile(int slot, out BotProfileData profile);

        // ---- buy plans ----

        // Force a bot's per-round buy.
        bool SetBuyPlan(int slot, string aliases);

        // Force a bot to buy nothing each round.
        bool SetBuySkip(int slot);

        // Remove a bot's buy plan (back to vanilla AI buying).
        bool ClearBuyPlan(int slot);

        bool ClearAllBuyPlans();

        // Plan item count: -1 none, 0 skip/empty, >0 alias count.
        int BuyPlanItemCount(int slot);

        // ---- voice ----

        // Returns true when the native plugin can send voice net messages.
        bool CanSendVoice();

        // Returns 0 when voice sending is ready, otherwise a negative setup code.
        int GetVoiceStatus();

        // Sends one encoded Opus voice frame to a recipient player slot.
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
