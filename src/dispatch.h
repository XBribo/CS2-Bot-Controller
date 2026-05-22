// Lock/Unlock API: sets per-slot lock state and asks Hooks to push the
// bot onto the locked weapon via the cached WeaponServices.

#pragma once

#include "lock_state.h"

class IVEngineServer2;

namespace BotWeaponLock
{
    namespace Dispatch
    {
        // Set by plugin.cpp Load(). Used by Commands/Hooks debug output.
        extern IVEngineServer2 *g_pEngine;

        // Returns 0 on success; <0 on error (see exports.cpp / spec).
        int Lock(int slot, LockTarget target);
        int Unlock(int slot);
        void UnlockAll();
        int  GetLock(int slot);   // returns LockTarget int value
    }
}
