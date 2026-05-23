// BotLocker dispatch: weapon-lock (per-slot SelectItem gate + one-shot
// switch) and bot-lock (per-slot CCSBot::Update freeze).

#include "dispatch.h"
#include "WeaponLockerState.h"
#include "WeaponLocker.h"
#include "BotLockerState.h"

#include <eiface.h>
#include <playerslot.h>

namespace BotLocker
{
    namespace Dispatch
    {
        IVEngineServer2 *g_pEngine = nullptr;

        int Lock(int slot, LockTarget target)
        {
            if (slot < 0 || slot >= WeaponLockerState::kMaxSlots) return -2;
            if (target == LockTarget::None)               return -2;

            // Order matters: set the lock first so the AI hooks start
            // blocking immediately, then ask hooks to perform the one-shot
            // switch via the cached WeaponServices pointer. The switch
            // itself goes through the original SelectItem (un-hooked) so
            // there's no recursion.
            WeaponLockerState::Set(slot, target);
            (void)WeaponLockerHooks::SwitchToLockTarget(slot);
            return 0;
        }

        int Unlock(int slot)
        {
            if (slot < 0 || slot >= WeaponLockerState::kMaxSlots) return -2;
            WeaponLockerState::Clear(slot);
            return 0;
        }

        void UnlockAll()
        {
            WeaponLockerState::ClearAll();
        }

        int GetLock(int slot)
        {
            return static_cast<int>(WeaponLockerState::Get(slot));
        }

        // ---- bot lock ----

        int LockBot(int slot)
        {
            if (slot < 0 || slot >= BotLockerState::kMaxSlots) return -2;
            BotLockerState::Set(slot, true);
            return 0;
        }

        int UnlockBot(int slot)
        {
            if (slot < 0 || slot >= BotLockerState::kMaxSlots) return -2;
            BotLockerState::Set(slot, false);
            return 0;
        }

        void UnlockAllBots()
        {
            BotLockerState::ClearAll();
        }

        int IsBotLocked(int slot)
        {
            if (slot < 0 || slot >= BotLockerState::kMaxSlots) return 0;
            return BotLockerState::Get(slot) ? 1 : 0;
        }
    }
}
