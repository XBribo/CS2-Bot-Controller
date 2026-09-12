// Detour for BuyState::OnUpdate

#include "BuyController.h"
#include "BuyControllerState.h"
#include "MotionRecorder.h"
#include "ccsbot_slot.h"
#include "nlohmann/json.hpp"
#include "sig_scan.h"
#include "version_targets.h"
#include "dispatch.h"
#include "hooks.h"

#include <eiface.h>
#include <playerslot.h>
#include <convar.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace tg = cs2bc::targets;

namespace cs2bc {
namespace buy_controller_hooks {

namespace {

void* g_addrOnUpdate = nullptr;

hooks::NativeHook<void, void*, void*> g_hookOnUpdate;

bool g_installed = false;

std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

// Per-slot last seen m_isInitialDelay, used for rising-edge detection.
//
// This state is runtime-only and must not survive Disable()/Pause().
uint8_t g_lastInitDelay[64] = { 0 };

// -----------------------------------------------------------------------------
// Clears transient per-slot state.
//
// Safe to call repeatedly.
// -----------------------------------------------------------------------------

void ClearRuntimeState()
{
    for (auto& value : g_lastInitDelay)
        value = 0;
}

// -----------------------------------------------------------------------------
// Runs "buy <alias>" server-side for a bot slot.
// -----------------------------------------------------------------------------

void IssueBuy(int slot, const char* alias)
{
    if (!dispatch::g_gameClients || slot < 0 || slot >= 64 || !alias)
    {
        return;
    }

    char line[128];

    std::snprintf(line, sizeof(line), "buy %s", alias);

    CCommand cmd;

    if (!cmd.Tokenize(line)) return;

    dispatch::g_gameClients->ClientCommand(CPlayerSlot(slot), cmd);
}

// -----------------------------------------------------------------------------
// Executes a slot's complete buy plan in one tick, then tells the vanilla
// BuyState that buying is complete.
// -----------------------------------------------------------------------------

void ApplyPlan(void* self, int slot)
{
    BuyPlan plan;

    if (!buy_controller_state::Copy(slot, plan))
    {
        return;
    }

    if (!plan.skip)
    {
        for (const auto& alias : plan.items)
        {
            IssueBuy(slot, alias.c_str());
        }
    }

    // Tell vanilla buying that it has finished so it exits the state.
    const uint8_t done = 1;

    WriteField(self, tg::g_buyDoneBuying, done);
}

// -----------------------------------------------------------------------------
// Applies the custom buy plan before vanilla advances its state.
// -----------------------------------------------------------------------------

KHook::Return<void> HookedOnUpdate(void* self, void* me) noexcept
{
    const int slot = CCSBotToSlot(me);

    // Replay owns the bot while active.
    if (slot >= 0 && slot < 64 && motion_recorder::IsReplaying(slot))
    {
        return { KHook::Action::Supersede };
    }

    if (slot < 0 || slot >= 64 || !buy_controller_state::HasPlan(slot))
    {
        return { KHook::Action::Ignore };
    }

    uint8_t initialDelay = 0;

    if (!SafeRead(self, tg::g_buyInitialDelay, initialDelay))
    {
        return { KHook::Action::Ignore };
    }

    // Rising edge of m_isInitialDelay means that this bot has freshly entered
    // BuyState for the current round.
    if (initialDelay && !g_lastInitDelay[slot])
    {
        ApplyPlan(self, slot);
    }

    g_lastInitDelay[slot] = initialDelay;

    return { KHook::Action::Ignore };
}

} // namespace

// -----------------------------------------------------------------------------
// Install
//
// Idempotent:
//
// Install(); Install();
//
// leaves exactly one hook installed.
// -----------------------------------------------------------------------------

bool Install(const nlohmann::json& gd, const sig::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen)
{
    // Critical double-install protection.
    if (g_installed)
    {
        g_status = "ok";
        return true;
    }

    // Defensive cleanup in case a previous installation failed partway
    // through.
    Remove();

    g_addrOnUpdate = nullptr;

    // ---------------------------------------------------------------------
    // Resolve BuyState::OnUpdate
    // ---------------------------------------------------------------------

    g_addrOnUpdate = sig::ResolveSig(gd, serverModule, "BuyState::OnUpdate", errorOut, errorOutLen);

    if (!g_addrOnUpdate)
    {
        // Keep the failure state after cleanup.
        Remove();

        g_status = "failed: OnUpdate sig";

        return false;
    }

    // ---------------------------------------------------------------------
    // Install hook
    // ---------------------------------------------------------------------

    if (!g_hookOnUpdate.Install(g_addrOnUpdate, &HookedOnUpdate))
    {
        // Remove any partial state before returning.
        Remove();

        if (errorOut && errorOutLen > 0)
        {
            std::snprintf(errorOut, errorOutLen, "hook BuyState::OnUpdate failed");
        }

        g_status = "failed: hook OnUpdate";

        return false;
    }

    // Start every activation cycle with clean edge-detection state.
    ClearRuntimeState();

    g_installed = true;
    g_status = "ok";

    return true;
}

// -----------------------------------------------------------------------------
// Remove
//
// Idempotent and safe after partial Install().
//
// We intentionally do NOT:
//
//     if (!g_installed) return;
//
// because Remove() is also used as rollback when Install() fails.
// -----------------------------------------------------------------------------

void Remove()
{
    g_hookOnUpdate.Remove();

    ClearRuntimeState();

    g_installed = false;
    g_status = "not_attempted";
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

const char* Status() { return g_status.c_str(); }

void* OnUpdateAddress() { return g_addrOnUpdate; }

} // namespace buy_controller_hooks
} // namespace cs2bc
