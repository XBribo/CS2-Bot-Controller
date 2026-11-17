// CS2 movement and usercmd hooks.

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include "core/cs2_sdk/src/core/memory_module.h"

namespace cs2bc {
namespace input_injector {
// Player slot capacity.
static constexpr int kMaxSlots = 64;

// Resolve sigs and install the movement hooks.
bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove the hooks.
void Remove();

const char* Status();

// Checks required recorder hooks.
bool RecorderReady();

// Registers the current Pawn for recording or replay.
bool SetReplayPawn(int slot, void* pawn);

// Detaches the pawn and its movement service.
void ClearReplayPawn(int slot);

// Resolves the registered live Pawn.
void* ResolveReplayPawn(int slot, void* services);

// Starts button injection; returns a token.
int64_t InjectUsercmd(int slot, uint64_t buttonMask, int durationMs);

// Cancels a button-injection token.
bool CancelUsercmdInjection(int slot, int64_t injectionId);

// Starts persistent movement; returns a token.
int64_t StartUsercmdMovement(int slot, float forwardMove, float leftMove);

// Updates a movement token.
bool UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove);

// Cancels a movement token.
bool CancelUsercmdMovement(int slot, int64_t movementId);

// Suppresses buttons for a fixed duration.
bool SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs);

// Starts persistent suppression; returns a token.
int64_t StartUsercmdSuppression(int slot, uint64_t buttonMask);

// Cancels a suppression token.
bool CancelUsercmdSuppression(int slot, int64_t suppressionId);

// Clears the slot's input controls.
void ClearUsercmdInjections(int slot);

} // namespace input_injector
} // namespace cs2bc
