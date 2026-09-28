// MinHook install/remove for CCSBot Update/Upkeep.

#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include "core/memory_module.h"

namespace cs2bc {
namespace bot_controller_hooks {
// Resolve sigs and install detours.
bool Install(const nlohmann::json& gd, const modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove detours.
void Remove();

const char* Status();

void* UpdateAddress();
void* UpkeepAddress();
void* UpdateLookAnglesAddress();

// Reports whether the engine view hooks required by replay are installed.
bool ReplayViewReady();

// Last CCSBot* seen in Update for this slot, or nullptr. Used to read
// the bot's BotProfile by slot.
void* BotForSlot(int slot);
} // namespace bot_controller_hooks
} // namespace cs2bc
