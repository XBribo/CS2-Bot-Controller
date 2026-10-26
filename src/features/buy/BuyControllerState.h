// Per-slot bot buy plan table

#pragma once

#include <string>
#include <vector>

namespace cs2bc {
/* One bot's buy plan: skip=true means buy nothing this round */
struct BuyPlan
{
    bool skip = false;
    std::vector<std::string> items; // ordered buy aliases
};

namespace buy_controller_state {
constexpr int kMaxSlots = 64;

// Checks for an item or skip plan.
bool HasPlan(int slot);

// Replace this slot's plan
void Set(int slot, const std::vector<std::string>& items, bool skip);

// Copies the plan; false if absent.
bool Copy(int slot, BuyPlan& out);

void Clear(int slot);
void ClearAll();

// Alias count: -1 no plan, 0 skip.
int ItemCount(int slot);

// Count of slots with a plan
int CountPlans();
} // namespace buy_controller_state
} // namespace cs2bc
