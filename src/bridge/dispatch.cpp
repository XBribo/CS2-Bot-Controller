// Lock dispatch: routes per LockKind to the right state table.

#include "dispatch.h"
#include "WeaponLockerState.h"
#include "WeaponLocker.h"
#include "BotControllerState.h"
#include "MotionRecorder.h"

#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>

namespace cs2bc {
namespace dispatch {
IVEngineServer2* g_engine = nullptr;
ISource2GameClients* g_gameClients = nullptr;

// Sets a lock; weapon locks also switch once.
int Lock(int slot, LockKind kind, int arg)
{
    if (slot < 0 || slot >= bot_controller_state::kMaxSlots) return -2;
    if (motion_recorder::IsReplaying(slot)) return -3;

    switch (kind)
    {
        case LockKind::All:
            bot_controller_state::SetAll(slot, true);
            return 0;

        case LockKind::Aim:
            bot_controller_state::SetAim(slot, true);
            return 0;

        case LockKind::Weapon:
        {
            if (arg < static_cast<int>(LockTarget::Slot1) || arg > static_cast<int>(LockTarget::Slot5)) return -2;
            const auto tgt = static_cast<LockTarget>(arg);
            weapon_locker_state::Set(slot, tgt);
            (void)weapon_locker_hooks::SwitchToLockTarget(slot);
            return 0;
        }
    }
    return -2;
}

// Clears the slot's lock.
int Unlock(int slot, LockKind kind)
{
    if (slot < 0 || slot >= bot_controller_state::kMaxSlots) return -2;
    switch (kind)
    {
        case LockKind::All:
            bot_controller_state::SetAll(slot, false);
            return 0;

        case LockKind::Aim:
            bot_controller_state::SetAim(slot, false);
            return 0;

        case LockKind::Weapon:
            weapon_locker_state::Clear(slot);
            return 0;
    }
    return -2;
}

// Clears this lock kind on all slots.
int UnlockAll(LockKind kind)
{
    switch (kind)
    {
        case LockKind::All:
            bot_controller_state::ClearAllAll();
            return 0;
        case LockKind::Aim:
            bot_controller_state::ClearAllAim();
            return 0;
        case LockKind::Weapon:
            weapon_locker_state::ClearAll();
            return 0;
    }
    return -2;
}

// Queries the slot's lock.
int IsLocked(int slot, LockKind kind)
{
    if (slot < 0 || slot >= bot_controller_state::kMaxSlots) return 0;
    switch (kind)
    {
        case LockKind::All:
            return bot_controller_state::GetAll(slot) ? 1 : 0;
        case LockKind::Aim:
            return bot_controller_state::GetAim(slot) ? 1 : 0;
        case LockKind::Weapon:
            return static_cast<int>(weapon_locker_state::Get(slot));
    }
    return 0;
}
} // namespace dispatch
} // namespace cs2bc
