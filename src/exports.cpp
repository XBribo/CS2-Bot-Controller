// C-ABI exports for CounterStrikeSharp P/Invoke. Main-thread only.

#include "dispatch.h"
#include "WeaponLockerState.h"

extern "C" __declspec(dllexport)
int BotLocker_Lock(int slot, int target)
{
    return BotLocker::Dispatch::Lock(slot,
        static_cast<BotLocker::LockTarget>(target));
}

extern "C" __declspec(dllexport)
int BotLocker_Unlock(int slot)
{
    return BotLocker::Dispatch::Unlock(slot);
}

extern "C" __declspec(dllexport)
void BotLocker_UnlockAll()
{
    BotLocker::Dispatch::UnlockAll();
}

extern "C" __declspec(dllexport)
int BotLocker_GetLock(int slot)
{
    return BotLocker::Dispatch::GetLock(slot);
}

extern "C" __declspec(dllexport)
int BotLocker_LockBot(int slot)
{
    return BotLocker::Dispatch::LockBot(slot);
}

extern "C" __declspec(dllexport)
int BotLocker_UnlockBot(int slot)
{
    return BotLocker::Dispatch::UnlockBot(slot);
}

extern "C" __declspec(dllexport)
void BotLocker_UnlockAllBots()
{
    BotLocker::Dispatch::UnlockAllBots();
}

extern "C" __declspec(dllexport)
int BotLocker_IsBotLocked(int slot)
{
    return BotLocker::Dispatch::IsBotLocked(slot);
}

extern "C" __declspec(dllexport)
int BotLocker_GetVersion()
{
    return 3;
}
