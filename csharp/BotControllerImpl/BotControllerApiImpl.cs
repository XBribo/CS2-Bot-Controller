// Provider-side implementation of IBotControllerApi.

namespace BotControllerApi
{
    public sealed class BotControllerApiImpl : IBotControllerApi
    {
        public int AbiVersion => BotController.AbiVersion;
        public ulong Capabilities => BotController.Capabilities();
        public string BuildId => BotController.BuildId();

        public bool TryGetAbiInfo(out AbiInfo info)
            => BotController.TryGetAbiInfo(out info);

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
        public (ReplayTick[] ticks, SubtickMove[] subs) GetRecordedMotion(int slot)
            => BotController.GetRecordedMotion(slot);

        // ---- replay ----
        public bool LoadReplay(int slot, ReplayTick[] ticks, SubtickMove[] subs)
            => BotController.LoadReplay(slot, ticks, subs);
        public bool LoadReplayExtended(
            int slot,
            ReplayTick[] ticks,
            SubtickMove[] subs,
            ReplayCommandFrame[] commands,
            ReplayMovementExtra[] movementExtras)
            => BotController.LoadReplayExtended(slot, ticks, subs, commands, movementExtras);
        public bool TransferRecordingToReplay(int srcSlot, int dstSlot)
            => BotController.TransferRecordingToReplay(srcSlot, dstSlot);
        public bool StartReplay(int slot, bool loop = false) => BotController.StartReplay(slot, loop);
        public bool StartReplayAt(int slot, bool loop, int startIndex)
            => BotController.StartReplayAt(slot, loop, startIndex);
        public bool StartReplayUntil(int slot, bool loop, int startIndex, int holdBeforeIndex)
            => BotController.StartReplayUntil(slot, loop, startIndex, holdBeforeIndex);
        public bool StopReplay(int slot) => BotController.StopReplay(slot);
        public int ReplayCursor(int slot) => BotController.ReplayCursor(slot);
        public int ReplayTotal(int slot) => BotController.ReplayTotal(slot);
        public bool IsReplaying(int slot) => BotController.IsReplaying(slot);
        public bool TryGetReplayTick(int slot, out ReplayTick tick)
            => BotController.TryGetReplayTick(slot, out tick);
        public bool TryGetReplaySlotState(int slot, out ReplaySlotState state)
            => BotController.TryGetReplaySlotState(slot, out state);
        public bool SetReplayPovMask(ulong mask) => BotController.SetReplayPovMask(mask);

        // ---- weapons ----
        public bool SwitchBotWeapon(int slot, int defIndex)
            => BotController.SwitchBotWeapon(slot, defIndex);
        public int BotActiveWeaponDef(int slot) => BotController.BotActiveWeaponDef(slot);

        // ---- profile ----
        public bool TryGetProfile(int slot, out BotProfileData profile)
            => BotController.TryGetProfile(slot, out profile);

        // ---- buy plans ----
        public bool SetBuyPlan(int slot, string aliases) => BotController.SetBuyPlan(slot, aliases);
        public bool SetBuySkip(int slot) => BotController.SetBuySkip(slot);
        public bool ClearBuyPlan(int slot) => BotController.ClearBuyPlan(slot);
        public bool ClearAllBuyPlans() => BotController.ClearAllBuyPlans();
        public int BuyPlanItemCount(int slot) => BotController.BuyPlanItemCount(slot);
    }
}
