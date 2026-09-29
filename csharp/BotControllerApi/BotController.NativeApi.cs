// P/Invoke wrapper for BotController.dll (ABI 23), check IsCompatible() before use
// Main-thread only.

using System.Runtime.InteropServices;

namespace BotControllerApi
{
    // Thin static binding over the native exports. No orchestration here.
    public static class BotController
    {
        private const int ExpectedAbiVersion = 23;

        // Native layout metadata, not a separately versioned public frame contract.
        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        private struct AbiInfo
        {
            public int Major, Minor, SnapshotSize, TickSize, SubtickSize, SlotStateSize, MaxSlots;
            public ulong Capabilities;
            public int FrameSize, InputSize;
        }

        // Sentinel weapon def meaning "any knife"
        public const int KnifeDef = 9001;

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_Lock(int slot, int kind, int arg);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_Unlock(int slot, int kind);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_UnlockAll(int kind);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_IsLocked(int slot, int kind);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetVersion();

        // Reject stale local ABI 23 builds before calling the unpublished frame signature.
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetAbiInfo(out AbiInfo info, int size);

        // Imports the native usercmd injection export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern long BotController_InjectUsercmd(
            int slot, ulong buttonMask, int durationMs);

        // Imports the persistent native analog movement export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern long BotController_StartUsercmdMovement(
            int slot, float forwardMove, float leftMove);

        // Imports the persistent native analog movement update export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_UpdateUsercmdMovement(
            int slot, long movementId, float forwardMove, float leftMove);

        // Imports the persistent native analog movement cancellation export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CancelUsercmdMovement(
            int slot, long movementId);

        // Imports the native usercmd injection cancellation export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CancelUsercmdInjection(
            int slot, long injectionId);

        // Imports the native usercmd suppression export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_SuppressUsercmd(
            int slot, ulong buttonMask, int durationMs);

        // Imports the persistent native usercmd suppression export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern long BotController_StartUsercmdSuppression(
            int slot, ulong buttonMask);

        // Imports the persistent native usercmd suppression cancellation export
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CancelUsercmdSuppression(
            int slot, long suppressionId);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_StartRecord(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_StopRecord(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetRecordedTickCount(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetRecordedSubtickCount(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetRecordedCommandCount(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CopyRecordedTicks(
            int slot, [Out] ReplayTick[] ticks, int maxTicks);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CopyRecordedTicksRange(
            int slot, int start, [Out] ReplayTick[] ticks, int maxTicks);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CopyRecordedSubticks(
            int slot, [Out] SubtickMove[] subs, int maxSubticks);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CopyRecordedSubticksRange(
            int slot, int start, [Out] SubtickMove[] subs, int maxSubticks);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CopyRecordedCommands(
            int slot, [Out] NativeReplayInput[] commands, int maxCommands);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CopyRecordedCommandsRange(
            int slot, int start, [Out] NativeReplayInput[] commands, int maxCommands);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_LoadReplay(
            int slot, float tickRate, [In] ReplayFrameData[] frames, int frameCount,
            [In] SubtickMove[] subs, int subCount);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_TransferRecordingToReplay(int srcSlot, int dstSlot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_StartReplay(int slot, int loop);

        // Starts or resumes at an inclusive replay index.
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_StartReplayAt(int slot, int loop, int startIndex);

        // Holds playback before an exclusive replay index.
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_StartReplayUntil(int slot, int loop, int startIndex, int holdBeforeIndex);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_SetReplayPawn(int slot, ulong pawnPtr);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_StopReplay(int slot);

        // Stops playback and releases all replay buffer allocations.
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_ReleaseReplayBuffer(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetReplayCursor(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetReplayTotal(int slot);

        // Reads the terminal cursor and completed-tick metadata in one native call.
        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetReplaySlotState(int slot, out ReplaySlotState state);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetReplayFrame(
            int slot, out ReplayFrameData frame, [Out] SubtickMove[] subs, int maxSubticks);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_SwitchBotWeapon(int slot, int defIndex);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetBotActiveWeaponDef(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetProfile(int slot, out BotProfileData profile);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_SetBuyPlan(int slot,
            [MarshalAs(UnmanagedType.LPStr)] string aliases);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_SetBuySkip(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_ClearBuyPlan(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_ClearAllBuyPlans();

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetBuyPlanItemCount(int slot);

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_CanSendVoice();

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_GetVoiceStatus();

        [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotController_SendVoiceFrame(
            int recipientSlot,
            int senderClient,
            ulong senderXuid,
            [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] byte[] audio,
            int audioBytes,
            int sampleRate,
            float voiceLevel,
            int sequenceBytes,
            int sectionNumber,
            int uncompressedSampleOffset,
            uint numPackets,
            [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 12)] uint[] packetOffsets,
            int packetOffsetCount,
            int tick,
            int audibleMask);

        // Native ABI must match what this wrapper expects.
        public static bool IsCompatible()
            => BotController_GetVersion() == ExpectedAbiVersion
               && BotController_GetAbiInfo(out var info, Marshal.SizeOf<AbiInfo>()) == 0
               && info.Major == ExpectedAbiVersion
               && info.SnapshotSize == Marshal.SizeOf<NativeMovementSnapshot>()
               && info.TickSize == Marshal.SizeOf<ReplayTick>()
               && info.SubtickSize == Marshal.SizeOf<SubtickMove>()
               && info.SlotStateSize == Marshal.SizeOf<ReplaySlotState>()
               && info.FrameSize == Marshal.SizeOf<ReplayFrameData>()
               && info.InputSize == Marshal.SizeOf<NativeReplayInput>();

        // Native C-ABI version the loaded DLL reports.
        public static int AbiVersion => BotController_GetVersion();

        // Creates an independently cancellable native usercmd injection
        public static long InjectUsercmd(int slot, ulong buttonMask, int durationMs = 0)
            => BotController_InjectUsercmd(slot, buttonMask, durationMs);

        // Cancels one native usercmd injection by its token
        public static bool CancelUsercmdInjection(int slot, long injectionId)
            => BotController_CancelUsercmdInjection(slot, injectionId) == 0;

        // Creates an independently cancellable persistent analog movement override
        public static long StartUsercmdMovement(
            int slot,
            float forwardMove,
            float leftMove)
            => BotController_StartUsercmdMovement(slot, forwardMove, leftMove);

        // Updates one persistent analog movement override
        public static bool UpdateUsercmdMovement(
            int slot,
            long movementId,
            float forwardMove,
            float leftMove)
            => BotController_UpdateUsercmdMovement(
                slot, movementId, forwardMove, leftMove) == 0;

        // Cancels one persistent analog movement override
        public static bool CancelUsercmdMovement(int slot, long movementId)
            => BotController_CancelUsercmdMovement(slot, movementId) == 0;

        // Suppresses selected usercmd buttons for a fixed duration
        public static bool SuppressUsercmd(int slot, ulong buttonMask, int durationMs)
            => BotController_SuppressUsercmd(slot, buttonMask, durationMs) == 0;

        // Creates an independently cancellable persistent native usercmd suppression
        public static long StartUsercmdSuppression(int slot, ulong buttonMask)
            => BotController_StartUsercmdSuppression(slot, buttonMask);

        // Cancels one persistent native usercmd suppression by its token
        public static bool CancelUsercmdSuppression(int slot, long suppressionId)
            => BotController_CancelUsercmdSuppression(slot, suppressionId) == 0;

        // ---- locks ----

        // All / Aim
        public static bool Lock(int slot, LockKind kind)
            => BotController_Lock(slot, (int)kind, 0) == 0;

        // Weapon: arg is the engine slot to lock onto
        public static bool Lock(int slot, LockTarget target)
            => BotController_Lock(slot, (int)LockKind.Weapon, (int)target) == 0;

        public static bool Unlock(int slot, LockKind kind)
            => BotController_Unlock(slot, (int)kind) == 0;

        public static bool UnlockAll(LockKind kind)
            => BotController_UnlockAll((int)kind) == 0;

        // For All/Aim returns true if locked; for Weapon use GetWeaponLock.
        public static bool IsLocked(int slot, LockKind kind)
            => BotController_IsLocked(slot, (int)kind) != 0;

        // Weapon-only query: returns the locked weapon slot, or None.
        public static LockTarget GetWeaponLock(int slot)
            => (LockTarget)BotController_IsLocked(slot, (int)LockKind.Weapon);

        // ---- recording ----

        public static bool StartRecord(int slot) => BotController_StartRecord(slot) == 0;

        public static bool StopRecord(int slot) => BotController_StopRecord(slot) == 0;

        public static int RecordedTickCount(int slot) => BotController_GetRecordedTickCount(slot);

        // Copies one fixed-size range from a stopped recording.
        internal static int CopyRecordedTicksRange(int slot, int start, ReplayTick[] ticks)
            => BotController_CopyRecordedTicksRange(slot, start, ticks, ticks.Length);

        // Copies one fixed-size subtick range from a stopped recording.
        internal static int CopyRecordedSubticksRange(int slot, int start, SubtickMove[] subs)
            => BotController_CopyRecordedSubticksRange(slot, start, subs, subs.Length);

        // Copies one fixed-size command range from a stopped recording.
        internal static int CopyRecordedCommandsRange(int slot, int start, NativeReplayInput[] commands)
            => BotController_CopyRecordedCommandsRange(slot, start, commands, commands.Length);

        // Reports the size of the stopped recording's subtick buffer.
        internal static int RecordedSubtickCount(int slot) => BotController_GetRecordedSubtickCount(slot);

        // Reports the size of the stopped recording's command buffer.
        internal static int RecordedCommandCount(int slot) => BotController_GetRecordedCommandCount(slot);

        // Convert a stopped recording without exposing parallel transport buffers.
        public static ReplayData GetRecordedMotion(int slot, float tickRate)
        {
            if (!IsCompatible()) throw new InvalidOperationException("BotController ABI mismatch.");
            var (ticks, subs, commands) = CopyRecordedBuffers(slot);
            return ReplayFrameCodec.FromRecording(tickRate, ticks, subs, commands);
        }

        // Pull aligned tick, subtick, and command-frame buffers from native memory
        private static (ReplayTick[] ticks, SubtickMove[] subs, NativeReplayInput[] commands)
            CopyRecordedBuffers(int slot)
        {
            int nt = BotController_GetRecordedTickCount(slot);
            if (nt <= 0)
                return (Array.Empty<ReplayTick>(), Array.Empty<SubtickMove>(), Array.Empty<NativeReplayInput>());

            var ticks = new ReplayTick[nt];
            int gotT = BotController_CopyRecordedTicks(slot, ticks, nt);
            if (gotT <= 0)
                return (Array.Empty<ReplayTick>(), Array.Empty<SubtickMove>(), Array.Empty<NativeReplayInput>());
            if (gotT != nt) Array.Resize(ref ticks, gotT);

            int ns = BotController_GetRecordedSubtickCount(slot);
            SubtickMove[] subs;
            if (ns <= 0)
                subs = Array.Empty<SubtickMove>();
            else
            {
                subs = new SubtickMove[ns];
                int gotS = BotController_CopyRecordedSubticks(slot, subs, ns);
                if (gotS <= 0) subs = Array.Empty<SubtickMove>();
                else if (gotS != ns) Array.Resize(ref subs, gotS);
            }

            int nc = BotController_GetRecordedCommandCount(slot);
            NativeReplayInput[] commands;
            if (nc <= 0)
                commands = Array.Empty<NativeReplayInput>();
            else
            {
                commands = new NativeReplayInput[nc];
                int gotC = BotController_CopyRecordedCommands(slot, commands, nc);
                if (gotC <= 0) commands = Array.Empty<NativeReplayInput>();
                else if (gotC != nc) Array.Resize(ref commands, gotC);
            }
            return (ticks, subs, commands);
        }

        // ---- replay ----

        // Translate one public model to the private packed native transport.
        public static bool LoadReplay(int slot, ReplayData replay)
        {
            if (replay?.Frames is not { Length: > 0 } || !IsCompatible() ||
                !ReplayFrameCodec.TryEncode(replay, out var frames, out var subs))
                return false;
            return BotController_LoadReplay(slot, replay.TickRate, frames, frames.Length, subs, subs.Length) == 0;
        }

        // Move a slot's just-recorded buffers straight into another slot's
        // replay buffer, no managed round-trip.
        public static bool TransferRecordingToReplay(int srcSlot, int dstSlot)
            => BotController_TransferRecordingToReplay(srcSlot, dstSlot) == 0;

        public static bool StartReplay(int slot, bool loop = false)
            => BotController_StartReplay(slot, loop ? 1 : 0) == 0;

        // Starts at an inclusive index, or resumes at the held boundary without reinitializing movement.
        public static bool StartReplayAt(int slot, bool loop, int startIndex)
            => BotController_StartReplayAt(slot, loop ? 1 : 0, startIndex) == 0;

        // Plays [startIndex, holdBeforeIndex), then holds input and retains replay ownership.
        public static bool StartReplayUntil(int slot, bool loop, int startIndex, int holdBeforeIndex)
            => BotController_StartReplayUntil(slot, loop ? 1 : 0, startIndex, holdBeforeIndex) == 0;

        // Registers the current native pawn before replay starts.
        public static bool SetReplayPawn(int slot, nint pawn)
            => pawn != 0 &&
               BotController_SetReplayPawn(slot, unchecked((ulong)pawn)) == 0;

        public static bool StopReplay(int slot) => BotController_StopReplay(slot) == 0;

        // Stops replay and frees its loaded buffers without affecting recorded motion.
        public static bool ReleaseReplayBuffer(int slot) => BotController_ReleaseReplayBuffer(slot) == 0;

        public static int ReplayCursor(int slot) => BotController_GetReplayCursor(slot);

        public static int ReplayTotal(int slot) => BotController_GetReplayTotal(slot);

        // Includes terminal state after natural completion or an explicit stop.
        public static bool TryGetReplayState(int slot, out ReplaySlotState state)
            => BotController_GetReplaySlotState(slot, out state) == 0;

        public static bool IsReplaying(int slot) => BotController_GetReplayCursor(slot) >= 0;

        // The tick currently being replayed on this slot, for driving weapon/fire
        // C#-side. Returns false if the slot isn't replaying.
        // Read input, snapshots, history and subticks from the same native frame.
        public static bool TryGetReplayFrame(int slot, out ReplayFrame frame)
        {
            frame = null!;
            if (!IsCompatible()) return false;
            var subs = new SubtickMove[36];
            if (BotController_GetReplayFrame(slot, out var data, subs, subs.Length) != 0)
                return false;
            Array.Resize(ref subs, checked((int)data.Tick.NumSubtick));
            frame = ReplayFrameCodec.Decode(data, subs);
            return true;
        }

        // Switch a bot to the weapon with this def index.
        public static bool SwitchBotWeapon(int slot, int defIndex)
            => BotController_SwitchBotWeapon(slot, defIndex) == 0;

        // Def index of the bot's current active weapon, same normalization as the
        // recorded WeaponDefIndex. <0 if unresolved.
        public static int BotActiveWeaponDef(int slot)
            => BotController_GetBotActiveWeaponDef(slot);

        // ---- profile ----

        // Read the BotProfile of the bot on this slot. Returns false if the slot
        // has no live bot (it must have ticked at least once) or null profile.
        public static bool GetBotProfile(int slot, out BotProfileData profile)
            => BotController_GetProfile(slot, out profile) == 0;

        // ---- buy plans ----

        // Force a bot's per-round buy.
        public static bool SetBuyPlan(int slot, string aliases)
            => BotController_SetBuyPlan(slot, aliases ?? "") == 0;

        // Force a bot to buy nothing each round.
        public static bool SetBuySkip(int slot)
            => BotController_SetBuySkip(slot) == 0;

        // Remove a bot's buy plan (back to vanilla AI buying).
        public static bool ClearBuyPlan(int slot)
            => BotController_ClearBuyPlan(slot) == 0;

        public static bool ClearAllBuyPlans()
            => BotController_ClearAllBuyPlans() == 0;

        // Plan item count: -1 none, 0 skip/empty, >0 alias count.
        public static int BuyPlanItemCount(int slot)
            => BotController_GetBuyPlanItemCount(slot);

        // ---- voice ----

        // Returns true when the native plugin can send voice net messages.
        public static bool CanSendVoice() => BotController_CanSendVoice() != 0;

        // Returns 0 when voice sending is ready, otherwise a negative setup code.
        public static int GetVoiceStatus() => BotController_GetVoiceStatus();

        // Sends one encoded Opus voice frame to a recipient player slot.
        public static int SendVoiceFrame(
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
        {
            audio ??= Array.Empty<byte>();
            packetOffsets ??= Array.Empty<uint>();
            if (audioBytes < 0 || audioBytes > audio.Length ||
                packetOffsetCount < 0 || packetOffsetCount > packetOffsets.Length)
                return -2;

            return BotController_SendVoiceFrame(
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
}
