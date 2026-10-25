// CS2 movement and command hooks (ProcessMovement / OnSimulateUserCommands / PlayerRunCommand).

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include "core/cs2_sdk/src/core/memory_module.h"

namespace cs2bc {
namespace input_injector {
// Max bots we track per-slot state for.
static constexpr int kMaxSlots = 64;

// Resolve sigs and install the movement hooks.
bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove the hooks.
void Remove();

const char* Status();

// Requires the frame-boundary and command hooks; logs unavailable capabilities.
bool RecorderReady();

// Registers the authoritative pawn supplied by the managed plugin for recording or replay.
bool SetReplayPawn(int slot, void* pawn);

// Detaches the pawn and its movement service.
void ClearReplayPawn(int slot);

// Resolves only a registered, current pawn.
void* ResolveReplayPawn(int slot, void* services);

// Creates an independently cancellable usercmd button injection
int64_t InjectUsercmd(int slot, uint64_t buttonMask, int durationMs);

// Cancels one usercmd injection by its token
bool CancelUsercmdInjection(int slot, int64_t injectionId);

// Creates an independently cancellable persistent analog movement override
int64_t StartUsercmdMovement(int slot, float forwardMove, float leftMove);

// Updates one persistent analog movement override
bool UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove);

// Cancels one persistent analog movement override
bool CancelUsercmdMovement(int slot, int64_t movementId);

// Suppresses selected usercmd buttons for a fixed duration
bool SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs);

// Creates an independently cancellable persistent usercmd suppression
int64_t StartUsercmdSuppression(int slot, uint64_t buttonMask);

// Cancels one persistent usercmd suppression by its token
bool CancelUsercmdSuppression(int slot, int64_t suppressionId);

// Clears every pending and active usercmd injection for one slot
void ClearUsercmdInjections(int slot);

} // namespace input_injector
} // namespace cs2bc
