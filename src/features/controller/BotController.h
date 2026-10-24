// MinHook install/remove for CCSBot Update/Upkeep.

#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include "core/cs2_sdk/src/core/memory_module.h"

namespace cs2bc {
namespace bot_controller_hooks {
// Resolve sigs and install detours.
bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove detours.
void Remove();

const char* Status();

void* UpdateAddress();
void* UpkeepAddress();
void* UpdateLookAnglesAddress();

// Publishes replay-owned eye angles through the current engine path.
bool ApplyReplayEyeAngles(void* pawn, float pitch, float yaw);
} // namespace bot_controller_hooks
} // namespace cs2bc
