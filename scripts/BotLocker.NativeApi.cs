// CSS wrapper for BotLocker.dll. DLL is loaded by Metamod, exports
// resolvable. Always check IsCompatible() before use. Main-thread only.

using System.Runtime.InteropServices;

namespace BotLockerApi
{
    // Engine weapon slots. Slot1=primary, Slot2=pistol, Slot3=knife/taser,
    // Slot4=grenades (he/flash/smoke/molotov/decoy share this slot),
    // Slot5=C4.
    public enum LockTarget
    {
        None = 0,
        Slot1 = 1,
        Slot2 = 2,
        Slot3 = 3,
        Slot4 = 4,
        Slot5 = 5,
    }

    public static class BotLocker
    {
        private const int ExpectedAbiVersion = 3;

        // ---- weapon lock ----
        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotLocker_Lock(int slot, int target);

        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotLocker_Unlock(int slot);

        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern void BotLocker_UnlockAll();

        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotLocker_GetLock(int slot);

        // ---- bot lock ----
        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotLocker_LockBot(int slot);

        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotLocker_UnlockBot(int slot);

        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern void BotLocker_UnlockAllBots();

        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotLocker_IsBotLocked(int slot);

        // ---- version ----
        [DllImport("BotLocker", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotLocker_GetVersion();

        public static bool IsCompatible() => BotLocker_GetVersion() == ExpectedAbiVersion;

        public static bool Lock(int slot, LockTarget t) => BotLocker_Lock(slot, (int)t) == 0;
        public static bool Unlock(int slot)             => BotLocker_Unlock(slot) == 0;
        public static void UnlockAll()                  => BotLocker_UnlockAll();
        public static LockTarget GetLock(int slot)      => (LockTarget)BotLocker_GetLock(slot);

        public static bool LockBot(int slot)            => BotLocker_LockBot(slot) == 0;
        public static bool UnlockBot(int slot)          => BotLocker_UnlockBot(slot) == 0;
        public static void UnlockAllBots()              => BotLocker_UnlockAllBots();
        public static bool IsBotLocked(int slot)        => BotLocker_IsBotLocked(slot) != 0;
    }
}
