// Detour for BuyState::OnUpdate to force a bot's per-round buy plan.

#pragma once

#include <nlohmann/json.hpp>
#include "core/cs2_sdk/src/core/memory_module.h"

namespace cs2bc {
namespace buy_controller_hooks {
// Resolve sig + offsets and install the detour.
bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

void Remove();

const char* Status();

void* OnUpdateAddress();
} // namespace buy_controller_hooks
} // namespace cs2bc
