// Unified lock dispatch. Per-slot lock kinds.

#pragma once

#include "WeaponLockerState.h"

class IVEngineServer2;
class ISource2GameClients;

namespace cs2bc {
// Mirror BotControllerApi.LockKind on the C# side.
enum class LockKind : int // NOLINT(performance-enum-size)
{
    All = 0,
    Aim = 1,
    Weapon = 2,
};

namespace dispatch {
extern IVEngineServer2* g_engine;
// Bot buy command executor.
extern ISource2GameClients* g_gameClients;

// arg = LockTarget int for Weapon kind
int Lock(int slot, LockKind kind, int arg);

int Unlock(int slot, LockKind kind);

int UnlockAll(LockKind kind);

// 1 if All/Aim locked; Weapon returns LockTarget int.
int IsLocked(int slot, LockKind kind);
} // namespace dispatch
} // namespace cs2bc
