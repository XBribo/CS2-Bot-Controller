// funchook for CS2 movement functions (ProcessMovement / PhysicsSimulate / FinishMove / PlayerRunCommand)

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include "sig_scan.h"

namespace BotController {
namespace InputInjector {
// Max bots we track per-slot state for.
static constexpr int kMaxSlots = 64;

// Resolve sigs and install the movement hooks.
bool Install(const nlohmann::json& gd, const Sig::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove the hooks.
void Remove();

const char* Status();

// Resolved address of the hooked function.
void* ProcessUsercmdAddress();

// Registers the authoritative replay pawn supplied by the managed plugin.
bool SetReplayPawn(int slot, void* pawn);

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

// Diagnostics
uint64_t HookCallCount();
int LastResolvedSlot();
uint64_t FinishMoveCallCount();
uint64_t PlayerRunCommandCallCount();
uint64_t UsercmdMovementApplyCount();
int LastUsercmdMovementSlot();
int LastUsercmdForwardMove();
int LastUsercmdLeftMove();
uint64_t PhysicsSimulateCallCount();
int LastPhysicsSlot();
uint64_t ReplayCommitCount();
uint64_t SlotResolveCallCount();
uint64_t SlotResolveFailureCount();
uintptr_t LastServices();
uintptr_t LastPawn();
uint32_t LastControllerHandle();
uint32_t LastOriginalControllerHandle();
int LastControllerIndex();
int LastOriginalControllerIndex();
int LastOwnerSlot();
} // namespace InputInjector
} // namespace BotController
