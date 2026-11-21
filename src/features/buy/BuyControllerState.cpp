#include "BuyControllerState.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

namespace cs2bc {
namespace buy_controller_state {

namespace {
struct Entry
{
    bool present = false;
    BuyPlan plan;
    uint32_t pawn{};
    float submitted{};
};

std::array<Entry, kMaxSlots> g_plans{};
std::mutex g_mu;
std::atomic<uint64_t> g_pending{};
uint64_t Bit(int slot) { return uint64_t{1} << slot; }

} // namespace

void Set(int slot, const std::vector<std::string>& items, bool skip)
{
    std::scoped_lock lk(g_mu);
    g_pending.fetch_and(~Bit(slot));
    g_plans[slot] = {true, {skip, items}};
}

bool Copy(int slot, BuyPlan& out)
{
    std::scoped_lock lk(g_mu);
    if (!g_plans[slot].present) return false;
    out = g_plans[slot].plan;
    return true;
}

bool SuppressesNative(int slot)
{
    std::scoped_lock lk(g_mu);
    return g_plans[slot].plan.skip;
}

uint64_t PendingSlots() { return g_pending.load(std::memory_order_relaxed); }

bool ReserveOnce(int slot, uint32_t pawn, const std::vector<std::string>& items, float now)
{
    if (!pawn || pawn == UINT32_MAX || items.empty()) return false;
    std::scoped_lock lk(g_mu);
    if (g_plans[slot].present) return false;
    g_plans[slot] = {true, {true, items}, pawn, now};
    g_pending.fetch_or(Bit(slot));
    return true;
}

Claim ClaimOnce(int slot, uint32_t pawn, float now, bool eligible, bool inZone, BuyPlan& out)
{
    std::scoped_lock lk(g_mu);
    if (!(g_pending.load() & Bit(slot))) return Claim::Wait;
    const auto& e = g_plans[slot];
    const float age = now - e.submitted;
    Claim result;
    if (pawn != e.pawn || !eligible || age < 0) result = Claim::Invalidated;
    else if (age > 3.0F) result = Claim::Expired;
    else if (age < 0.25F || !inZone) return Claim::Wait;
    else { out = e.plan; result = Claim::Dispatch; }
    g_pending.fetch_and(~Bit(slot));
    return result;
}

void Clear(int slot)
{
    std::scoped_lock lk(g_mu);
    g_pending.fetch_and(~Bit(slot));
    g_plans[slot] = Entry{};
}

void ClearAll()
{
    std::scoped_lock lk(g_mu);
    g_pending.store(0);
    for (auto& e : g_plans)
        e = Entry{};
}

int ItemCount(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return -1;
    std::scoped_lock lk(g_mu);
    if (!g_plans[slot].present) return -1;
    return static_cast<int>(g_plans[slot].plan.items.size());
}

int CountPlans()
{
    std::scoped_lock lk(g_mu);
    int n = 0;
    for (auto& e : g_plans)
        if (e.present) ++n;
    return n;
}
} // namespace buy_controller_state
} // namespace cs2bc
