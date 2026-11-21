// BuyState::OnUpdate hook for custom buy plans.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <cs2_sdk/memory_module.h>

namespace cs2bc {
namespace buy_controller_hooks {
// Resolve sig + offsets and install the detour.
bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

bool ReserveOnce(int slot, uint32_t pawn, const std::vector<std::string>& items);
void UpdatePending(void* bot);
void Remove();

const char* Status();

void* OnUpdateAddress();
} // namespace buy_controller_hooks
} // namespace cs2bc
