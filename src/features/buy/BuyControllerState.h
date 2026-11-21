#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cs2bc {
struct BuyPlan
{
    bool skip = false;
    std::vector<std::string> items; // ordered buy aliases
};

namespace buy_controller_state {
constexpr int kMaxSlots = 64;

void Set(int slot, const std::vector<std::string>& items, bool skip);
bool Copy(int slot, BuyPlan& out);
bool SuppressesNative(int slot);

bool ReserveOnce(int slot, uint32_t pawn, const std::vector<std::string>& items, float now);
uint64_t PendingSlots();
enum class Claim { Wait, Dispatch, Expired, Invalidated };
Claim ClaimOnce(int slot, uint32_t pawn, float now, bool eligible, bool inZone, BuyPlan& out);

void Clear(int slot);
void ClearAll();

// -1 if absent.
int ItemCount(int slot);

int CountPlans();
} // namespace buy_controller_state
} // namespace cs2bc
