// C-ABI exports for CounterStrikeSharp P/Invoke. quiet=true on all entries.

#include "dispatch.h"

extern "C" __declspec(dllexport)
int BotLocker_Lock(int slot, int kind, int arg)
{
    return BotLocker::Dispatch::Lock(slot,
        static_cast<BotLocker::LockKind>(kind), arg, /*quiet=*/true);
}

extern "C" __declspec(dllexport)
int BotLocker_Unlock(int slot, int kind)
{
    return BotLocker::Dispatch::Unlock(slot,
        static_cast<BotLocker::LockKind>(kind), /*quiet=*/true);
}

extern "C" __declspec(dllexport)
int BotLocker_UnlockAll(int kind)
{
    return BotLocker::Dispatch::UnlockAll(
        static_cast<BotLocker::LockKind>(kind), /*quiet=*/true);
}

extern "C" __declspec(dllexport)
int BotLocker_IsLocked(int slot, int kind)
{
    return BotLocker::Dispatch::IsLocked(slot,
        static_cast<BotLocker::LockKind>(kind));
}

extern "C" __declspec(dllexport)
int BotLocker_GetVersion()
{
    return 5;
}
