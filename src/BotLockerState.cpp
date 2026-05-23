// Per-slot bot lock flag.

#include "BotLockerState.h"

#include <array>
#include <atomic>

namespace BotLocker
{
    namespace BotLockerState
    {
        static std::array<std::atomic<bool>, kMaxSlots> g_locks{};

        bool Get(int slot)
        {
            if (slot < 0 || slot >= kMaxSlots) return false;
            return g_locks[slot].load(std::memory_order_relaxed);
        }

        void Set(int slot, bool locked)
        {
            if (slot < 0 || slot >= kMaxSlots) return;
            g_locks[slot].store(locked, std::memory_order_relaxed);
        }

        void ClearAll()
        {
            for (auto &x : g_locks) x.store(false, std::memory_order_relaxed);
        }

        int CountLocked()
        {
            int n = 0;
            for (auto &x : g_locks)
                if (x.load(std::memory_order_relaxed)) ++n;
            return n;
        }
    }
}
