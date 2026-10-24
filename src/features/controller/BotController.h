// Bot AI and eye-angle hooks.

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

// Reports whether native replay eye-angle publishing is available.
bool ReplayViewReady();

// Publishes eye angles or stops the affected replay on failure.
bool ApplyReplayEyeAngles(int slot, void* pawn, float pitch, float yaw);
} // namespace bot_controller_hooks
} // namespace cs2bc
