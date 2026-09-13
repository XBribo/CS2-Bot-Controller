// KHook install/remove for CCSBot Update/Upkeep and replay view control.

#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include "sig_scan.h"

namespace cs2bc {
namespace bot_controller_hooks {
// Resolve sigs and install detours.
bool Install(const nlohmann::json& gd, const sig::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove detours.
void Remove();

const char* Status();

void* UpdateAddress();
void* UpkeepAddress();
void* UpdateLookAnglesAddress();

// Publishes replay-owned eye angles through the current engine path.
bool ApplyReplayEyeAngles(void* pawn, float pitch, float yaw);

// Last CCSBot* seen in Update for this slot, or nullptr. Used to read
// the bot's BotProfile by slot.
void* BotForSlot(int slot);

// Returns true only when this slot is currently backed by a live CCSBot.
// The cached bot pointer is revalidated before returning true.
bool IsLiveBotSlot(int slot);
} // namespace bot_controller_hooks
} // namespace cs2bc
