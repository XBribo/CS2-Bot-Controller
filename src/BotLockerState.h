// Per-slot bot lock flag. O(1), thread-safe.

#pragma once

namespace BotLocker
{
    namespace BotLockerState
    {
        constexpr int kMaxSlots = 64;

        bool Get(int slot);
        void Set(int slot, bool locked);
        void ClearAll();

        // Returns count of currently locked slots.
        int  CountLocked();
    }
}
