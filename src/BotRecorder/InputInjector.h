// funchook for CS2 movement functions (ProcessMovement / PhysicsSimulate / FinishMove / PlayerRunCommand)

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include "sig_scan.h"

namespace BotController
{
    namespace InputInjector
    {
        // Max bots we track per-slot state for.
        static constexpr int kMaxSlots = 64;

        // Resolve sigs and install the movement hooks.
        bool Install(const nlohmann::json &gd, const Sig::ModuleInfo &serverModule,
                     char *errorOut, size_t errorOutLen);

        // Disable + remove the hooks.
        void Remove();

        const char *Status();

        // Resolved address of the hooked function.
        void *ProcessUsercmdAddress();

        // Registers the authoritative replay pawn supplied by the managed plugin.
        bool SetReplayPawn(int slot, void *pawn);

        // Clears the registered replay pawn for a slot.
        void ClearReplayPawn(int slot);

        // Resolves and validates the pawn owning the supplied movement services.
        void *ResolveReplayPawn(int slot, void *services);

        // Queues one usercmd button press followed by its release
        bool PulseUsercmdButton(int slot, uint64_t buttonMask);

        // Diagnostics
        uint64_t HookCallCount();
        int LastResolvedSlot();
        uint64_t FinishMoveCallCount();
        uint64_t PlayerRunCommandCallCount();
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
    }
}
