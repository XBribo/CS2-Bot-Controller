// Per-slot bot lock flags.

#include "BotControllerState.h"

#include <atomic>
#include <bit>
#include <cstdint>

namespace cs2bc {
namespace bot_controller_state {

namespace {
std::atomic<uint64_t> g_allLocks{ 0 };
std::atomic<uint64_t> g_aimLocks{ 0 };

} // namespace

bool HasAnyLock() { return g_allLocks.load(std::memory_order_relaxed) != 0 || g_aimLocks.load(std::memory_order_relaxed) != 0; }

bool GetAll(int slot) { return (g_allLocks.load(std::memory_order_relaxed) & (uint64_t{ 1 } << slot)) != 0; }

void SetAll(int slot, bool locked)
{
    const uint64_t bit = uint64_t{ 1 } << slot;
    if (locked) g_allLocks.fetch_or(bit, std::memory_order_relaxed);
    else
        g_allLocks.fetch_and(~bit, std::memory_order_relaxed);
}

void ClearAllAll() { g_allLocks.store(0, std::memory_order_relaxed); }

int CountAll() { return std::popcount(g_allLocks.load(std::memory_order_relaxed)); }

bool GetAim(int slot) { return (g_aimLocks.load(std::memory_order_relaxed) & (uint64_t{ 1 } << slot)) != 0; }

void SetAim(int slot, bool locked)
{
    const uint64_t bit = uint64_t{ 1 } << slot;
    if (locked) g_aimLocks.fetch_or(bit, std::memory_order_relaxed);
    else
        g_aimLocks.fetch_and(~bit, std::memory_order_relaxed);
}

void ClearAllAim() { g_aimLocks.store(0, std::memory_order_relaxed); }

int CountAim() { return std::popcount(g_aimLocks.load(std::memory_order_relaxed)); }
} // namespace bot_controller_state
} // namespace cs2bc
