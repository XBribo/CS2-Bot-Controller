// Detour for BuyState::OnUpdate to force a bot's per-round buy plan.

#pragma once

#include <nlohmann/json.hpp>
#include "sig_scan.h"

namespace BotController {
namespace BuyControllerHooks {
// Resolve sig + offsets and install the detour.
bool Install(const nlohmann::json& gd, const Sig::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

void Remove();

const char* Status();

void* OnUpdateAddress();
} // namespace BuyControllerHooks
} // namespace BotController
