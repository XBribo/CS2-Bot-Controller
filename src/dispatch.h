// Lock/Unlock API: per-slot weapon lock (sets state and pushes the bot onto
// the locked weapon via the cached WeaponServices) and per-slot bot lock
// (freezes CCSBot::Update decisions for that slot).

#pragma once

#include "WeaponLockerState.h"

class IVEngineServer2;

namespace BotLocker
{
    namespace Dispatch
    {
        // Set by plugin.cpp Load(). Used by Commands/Hooks debug output.
        extern IVEngineServer2 *g_pEngine;

        // ---- weapon lock (per-slot, target = which weapon slot) ----
        // Returns 0 on success; <0 on error (see exports.cpp / spec).
        int Lock(int slot, LockTarget target);
        int Unlock(int slot);
        void UnlockAll();
        int  GetLock(int slot);   // returns LockTarget int value

        // ---- bot lock (per-slot, on/off; freezes AI decisions) ----
        // Returns 0 on success; <0 on error.
        int  LockBot(int slot);
        int  UnlockBot(int slot);
        void UnlockAllBots();
        int  IsBotLocked(int slot);  // returns 0 / 1
    }
}
