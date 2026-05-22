// CSS wrapper for BotWeaponLock.dll. DLL is loaded by Metamod, exports
// resolvable. Always check IsCompatible() before use. Main-thread only.

using System.Runtime.InteropServices;

namespace BotWeaponLockApi
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

    public static class BotWeaponLock
    {
        private const int ExpectedAbiVersion = 2;

        [DllImport("BotWeaponLock", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotWeaponLock_Lock(int slot, int target);

        [DllImport("BotWeaponLock", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotWeaponLock_Unlock(int slot);

        [DllImport("BotWeaponLock", CallingConvention = CallingConvention.Cdecl)]
        private static extern void BotWeaponLock_UnlockAll();

        [DllImport("BotWeaponLock", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotWeaponLock_GetLock(int slot);

        [DllImport("BotWeaponLock", CallingConvention = CallingConvention.Cdecl)]
        private static extern int BotWeaponLock_GetVersion();

        public static bool IsCompatible() => BotWeaponLock_GetVersion() == ExpectedAbiVersion;
        public static bool Lock(int slot, LockTarget t) => BotWeaponLock_Lock(slot, (int)t) == 0;
        public static bool Unlock(int slot) => BotWeaponLock_Unlock(slot) == 0;
        public static void UnlockAll() => BotWeaponLock_UnlockAll();
        public static LockTarget GetLock(int slot) => (LockTarget)BotWeaponLock_GetLock(slot);
    }
}
