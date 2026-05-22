// BotWeaponLock dispatch: Lock/Unlock + active weapon switch via direct
// invocation of the original CCSPlayer_WeaponServices::SelectItem (bot
// clients don't process slotN client commands).

#include "dispatch.h"
#include "lock_state.h"
#include "hooks.h"

#include <eiface.h>
#include <playerslot.h>

namespace BotWeaponLock
{
    namespace Dispatch
    {
        IVEngineServer2 *g_pEngine = nullptr;

        int Lock(int slot, LockTarget target)
        {
            if (slot < 0 || slot >= LockState::kMaxSlots) return -2;
            if (target == LockTarget::None)               return -2;

            // Order matters: set the lock first so the AI hooks start
            // blocking immediately, then ask hooks to perform the one-shot
            // switch via the cached WeaponServices pointer. The switch
            // itself goes through the original SelectItem (un-hooked) so
            // there's no recursion.
            LockState::Set(slot, target);
            (void)Hooks::SwitchToLockTarget(slot);
            return 0;
        }

        int Unlock(int slot)
        {
            if (slot < 0 || slot >= LockState::kMaxSlots) return -2;
            LockState::Clear(slot);
            return 0;
        }

        void UnlockAll()
        {
            LockState::ClearAll();
        }

        int GetLock(int slot)
        {
            return static_cast<int>(LockState::Get(slot));
        }
    }
}
