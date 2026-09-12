// Lock dispatch: routes per LockKind to the right state table.

#include "dispatch.h"
#include "runtime.h"
#include "BotController.h"
#include "WeaponLockerState.h"
#include "WeaponLocker.h"
#include "BotControllerState.h"
#include "MotionRecorder.h"

#include <eiface.h>

namespace cs2bc {
namespace dispatch {
IVEngineServer2* g_engine = nullptr;
ISource2GameClients* g_gameClients = nullptr;

// Set lock; Weapon also triggers a one-shot switch.
int Lock(int slot, LockKind kind, int arg)
{
    if (!runtime::IsEnabled()) return -10;

    if (!bot_controller_hooks::IsLiveBotSlot(slot)) return -4;

    if (motion_recorder::IsReplaying(slot)) return -3;

    switch (kind)
    {
        case LockKind::All:
            if (slot < 0 || slot >= bot_controller_state::kMaxSlots) return -2;
            bot_controller_state::SetAll(slot, true);
            return 0;

        case LockKind::Aim:
            if (slot < 0 || slot >= bot_controller_state::kMaxSlots) return -2;
            bot_controller_state::SetAim(slot, true);
            return 0;

        case LockKind::Weapon:
        {
            if (slot < 0 || slot >= weapon_locker_state::kMaxSlots) return -2;
            const auto tgt = static_cast<LockTarget>(arg);
            if (tgt == LockTarget::None) return -2;
            weapon_locker_state::Set(slot, tgt);
            (void)weapon_locker_hooks::SwitchToLockTarget(slot);
            return 0;
        }
    }
    return -2;
}

// Clear the per-kind lock for this slot.
int Unlock(int slot, LockKind kind)
{
    switch (kind)
    {
        case LockKind::All:
            if (slot < 0 || slot >= bot_controller_state::kMaxSlots) return -2;
            bot_controller_state::SetAll(slot, false);
            return 0;

        case LockKind::Aim:
            if (slot < 0 || slot >= bot_controller_state::kMaxSlots) return -2;
            bot_controller_state::SetAim(slot, false);
            return 0;

        case LockKind::Weapon:
            if (slot < 0 || slot >= weapon_locker_state::kMaxSlots) return -2;
            weapon_locker_state::Clear(slot);
            return 0;
    }
    return -2;
}

// Clear every slot under kind.
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

// Return lock state for this slot under kind.
int IsLocked(int slot, LockKind kind)
{
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
