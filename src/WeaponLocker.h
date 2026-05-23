// MinHook install/remove for CCSBot::EquipBestWeapon / EquipPistol and
// CCSPlayer_WeaponServices::SelectItem. Detours consult LockState; locked
// slot -> block AI weapon switching at every funnel.

#pragma once

#include <string>

namespace BotLocker
{
    namespace WeaponLockerHooks
    {
        // Install both hooks. gamedataPath: absolute path to gamedata.json.
        // serverIface: any server-side interface pointer (used to locate the
        // real server.dll past Metamod's shim).
        // Returns true on success. On failure, writes a message to errorOut.
        bool Install(const std::string &gamedataPath,
                     void *serverIface,
                     char *errorOut, size_t errorOutLen);

        // Disable + remove + uninitialize MinHook.
        void Remove();

        // Diagnostic: "ok" / "not_attempted" / "failed: <reason>".
        const char *Status();

        // Diagnostic: function addresses found (nullptr if not installed).
        void *EquipBestWeaponAddress();
        void *EquipPistolAddress();
        void *SelectItemAddress();
        void *GetSlotAddress();

        // Force the bot at `slot` to switch to its locked weapon (if any
        // is set in LockState). Called by dispatch after Lock() so the bot
        // immediately ends up holding the right weapon instead of waiting
        // for its AI to try a (now-blocked) switch.
        // Returns: 0 ok / 1 no ws cached yet (bot hasn't ticked) /
        // 2 no target weapon found / 3 hooks not installed.
        int SwitchToLockTarget(int slot);
    }
}
