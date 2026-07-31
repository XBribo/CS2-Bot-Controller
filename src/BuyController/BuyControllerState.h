// Per-slot bot buy plan table

#pragma once

#include <string>
#include <vector>

namespace BotController {
/* One bot's buy plan: skip=true means buy nothing this round */
struct BuyPlan
{
    bool skip = false;
    std::vector<std::string> items; // ordered buy aliases
};

namespace BuyControllerState {
constexpr int kMaxSlots = 64;

// True if this slot has any plan set (skip or item list)
bool HasPlan(int slot);

// Replace this slot's plan
void Set(int slot, const std::vector<std::string>& items, bool skip);

// Snapshot this slot's plan into out; false if none
bool Copy(int slot, BuyPlan& out);

void Clear(int slot);
void ClearAll();

// Number of buy aliases for this slot (-1 if no plan, skip plan = 0)
int ItemCount(int slot);

// Count of slots with a plan
int CountPlans();
} // namespace BuyControllerState
} // namespace BotController
