// KHook for CS2 movement functions (ProcessMovement / PhysicsSimulate / PlayerRunCommand)

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include "core/memory_module.h"

namespace cs2bc {
namespace input_injector {
// Max bots we track per-slot state for.
static constexpr int kMaxSlots = 64;

// Resolve sigs and install the movement hooks.
bool Install(const nlohmann::json& gd, const modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove the hooks.
void Remove();

const char* Status();

// Requires the frame-boundary and command hooks; logs unavailable capabilities.
bool RecorderReady();

// Registers the authoritative pawn supplied by the managed plugin for recording or replay.
bool SetReplayPawn(int slot, void* pawn);

// Caches validated services at registration so the first active tick has a frame boundary.
void PrimeSlotServices(int slot, void* services);

// Clears the registered replay pawn for a slot.
void ClearReplayPawn(int slot);

// Resolves and validates the pawn owning the supplied movement services.
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
