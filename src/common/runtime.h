// BotController runtime lifecycle.
//
// Keeps the native plugin loaded while allowing all runtime hooks and
// transient bot/record/replay state to be enabled or completely disabled.

#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

#include "sig_scan.h"

namespace cs2bc::runtime {

// Stores the data required to install runtime hooks later.
//
// Does NOT install any hooks.
//
// Safe to call once after gamedata has been loaded and the server module
// has been resolved.
bool Prepare(const nlohmann::json& gamedata, const cs2bc::sig::ModuleInfo& serverModule, char* error, size_t maxlen);

// Installs all BotController runtime hooks.
//
// Idempotent:
//   Enable() while already enabled -> success, no duplicate hooks.
bool Enable(char* error, size_t maxlen);

// Removes all runtime hooks and clears transient state.
//
// Idempotent:
//   Disable() while already disabled -> safe no-op.
void Disable();

// Enables or disables the runtime.
//
// Convenience function intended for the C ABI / managed provider.
bool SetEnabled(bool enabled, char* error, size_t maxlen);

// Returns true once Prepare() has successfully completed.
bool IsPrepared();

// Returns true while runtime hooks are installed.
bool IsEnabled();

// Full runtime shutdown.
//
// Disables hooks, clears transient state and forgets the prepared
// gamedata/module information.
//
// Intended for MetaMod plugin Unload().
void Shutdown();

} // namespace cs2bc::runtime
