// Per-slot lock target table.

#include "WeaponLockerState.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

namespace cs2bc {
namespace weapon_locker_state {

namespace {
std::array<std::atomic<int>, kMaxSlots> g_locks{};
std::atomic<uint64_t> g_lockedSlots{ 0 };
static_assert(kMaxSlots <= 64);

} // namespace

bool HasAnyLock() { return g_lockedSlots.load(std::memory_order_acquire) != 0; }

LockTarget Get(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return LockTarget::None;
    return static_cast<LockTarget>(g_locks[slot].load(std::memory_order_relaxed));
}

void Set(int slot, LockTarget tgt)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    g_locks[slot].store(static_cast<int>(tgt), std::memory_order_relaxed);
    const uint64_t bit = uint64_t{ 1 } << slot;
    if (tgt != LockTarget::None) g_lockedSlots.fetch_or(bit, std::memory_order_release);
    else
        g_lockedSlots.fetch_and(~bit, std::memory_order_release);
}

void Clear(int slot) { Set(slot, LockTarget::None); }

void ClearAll()
{
    for (auto& x : g_locks)
        x.store(0, std::memory_order_relaxed);
    g_lockedSlots.store(0, std::memory_order_release);
}

int CountLocked() { return std::popcount(g_lockedSlots.load(std::memory_order_acquire)); }
} // namespace weapon_locker_state
} // namespace cs2bc
