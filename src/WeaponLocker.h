// MinHook for CCSBot EquipBestWeapon/EquipPistol + WeaponServices::SelectItem.

#pragma once

#include <string>

namespace BotLocker
{
    namespace WeaponLockerHooks
    {
        bool Install(const std::string &gamedataPath,
                     void *serverIface,
                     char *errorOut, size_t errorOutLen);

        void Remove();

        const char *Status();

        void *EquipBestWeaponAddress();
        void *EquipPistolAddress();
        void *SelectItemAddress();
        void *GetSlotAddress();

        // Force bot at `slot` to its locked weapon. quiet skips DebugLine.
        // Returns: 0 ok / 1 no ws / 2 no target / 3 hooks not installed.
        int SwitchToLockTarget(int slot, bool quiet = false);
    }
}
