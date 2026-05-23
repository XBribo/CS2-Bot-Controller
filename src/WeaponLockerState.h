// Per-slot lock target table. O(1), thread-safe.

#pragma once

namespace BotLocker
{
    // Engine weapon slots (matches CCSPlayer_WeaponServices::GetSlot
    // 0-based index + 1, i.e. the classic slot1/slot2/slot3 convention).
    // Slot1 = primary, Slot2 = pistol/taser, Slot3 = knife,
    // Slot4 = grenades (he/flash/smoke/molotov/decoy share this slot),
    // Slot5 = C4.
    enum class LockTarget : int
    {
        None  = 0,
        Slot1 = 1,
        Slot2 = 2,
        Slot3 = 3,
        Slot4 = 4,
        Slot5 = 5,
    };

    namespace WeaponLockerState
    {
        constexpr int kMaxSlots = 64;

        LockTarget Get(int slot);
        void       Set(int slot, LockTarget tgt);
        void       Clear(int slot);
        void       ClearAll();

        // Returns count of currently locked slots.
        int  CountLocked();
    }
}
