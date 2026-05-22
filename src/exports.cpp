// C-ABI exports for CounterStrikeSharp P/Invoke. Main-thread only.

#include "dispatch.h"
#include "lock_state.h"

extern "C" __declspec(dllexport)
int BotWeaponLock_Lock(int slot, int target)
{
    return BotWeaponLock::Dispatch::Lock(slot,
        static_cast<BotWeaponLock::LockTarget>(target));
}

extern "C" __declspec(dllexport)
int BotWeaponLock_Unlock(int slot)
{
    return BotWeaponLock::Dispatch::Unlock(slot);
}

extern "C" __declspec(dllexport)
void BotWeaponLock_UnlockAll()
{
    BotWeaponLock::Dispatch::UnlockAll();
}

extern "C" __declspec(dllexport)
int BotWeaponLock_GetLock(int slot)
{
    return BotWeaponLock::Dispatch::GetLock(slot);
}

extern "C" __declspec(dllexport)
int BotWeaponLock_GetVersion()
{
    return 2;
}
