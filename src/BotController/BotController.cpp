// CCSBot Update/Upkeep detours

#include "BotController.h"
#include "BotControllerState.h"
#include "ccsbot_slot.h"
#include "nlohmann/json.hpp"
#include "sig_scan.h"
#include "MotionRecorder.h"
#include "version_targets.h"
#include "hooks.h"

#include <tier0/dbg.h>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace tg = cs2bc::targets;

namespace cs2bc {
namespace bot_controller_hooks {

namespace {

void* g_addrUpdate = nullptr;
void* g_addrUpkeep = nullptr;
void* g_addrUpdateLookAngles = nullptr;
void* g_addrSetEyeAngles = nullptr;

#ifdef _WIN32
void** g_entityIdentityChunks = nullptr;
#endif

bool g_installed = false;

std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

// slot -> last CCSBot* seen in Update
//
// Used for profile reads by slot.
void* g_slotToBot[64] = { nullptr };
std::mutex g_slotToBotMu;

hooks::NativeHook<void, void*> g_hookUpdate;
hooks::NativeHook<void, void*> g_hookUpkeep;
hooks::NativeHook<void, void*> g_hookUpdateLookAngles;

hooks::NativeHook<void, void*, float*> g_hookSetEyeAngles;

// -----------------------------------------------------------------------------
// Clears cached CCSBot pointers.
//
// These pointers are only valid while the runtime is active. They must not
// survive Disable()/Pause(), because player slots/entities may be reused.
// -----------------------------------------------------------------------------

void ClearSlotBotCache()
{
    std::scoped_lock lk(g_slotToBotMu);

    for (auto& bot : g_slotToBot)
        bot = nullptr;
}

// -----------------------------------------------------------------------------
// Normalizes an angle to the engine's expected [-180, 180) range.
// -----------------------------------------------------------------------------

float NormalizeDeg(float angle)
{
    angle = std::fmod(angle + 180.0F, 360.0F);

    if (angle < 0.0F) angle += 360.0F;

    return angle - 180.0F;
}

#ifdef _WIN32

// -----------------------------------------------------------------------------
// Resolves the entity identity chunk pointer referenced by SetEyeAngles.
// -----------------------------------------------------------------------------

void ResolveSetEyeAnglesEntityChunks(void* setEyeAngles)
{
    g_entityIdentityChunks = nullptr;

    if (!setEyeAngles) return;

    constexpr size_t kSearchBytes = 0x120;

    uint8_t code[kSearchBytes] = {};

    if (!TryReadMemory(setEyeAngles, 0, code, sizeof(code)))
    {
        return;
    }

    auto* functionBase = reinterpret_cast<uint8_t*>(setEyeAngles);

    for (size_t i = 0; i + 10 <= kSearchBytes; ++i)
    {
        if (code[i] != 0x4C || code[i + 1] != 0x8B || code[i + 2] != 0x05 || code[i + 7] != 0x4D || code[i + 8] != 0x85 ||
            code[i + 9] != 0xC0)
        {
            continue;
        }

        int32_t relative = 0;

        std::memcpy(&relative, code + i + 3, sizeof(relative));

        g_entityIdentityChunks = reinterpret_cast<void**>(functionBase + i + 7 + relative);

        return;
    }
}

// -----------------------------------------------------------------------------
// Resolves the live controller owning a replay pawn through entity chunks.
// -----------------------------------------------------------------------------

void* ReplayControllerForPawn(void* pawn)
{
    if (!pawn || !g_entityIdentityChunks)
    {
        return nullptr;
    }

    uint32_t handle = 0;

    if (!SafeRead(pawn, tg::g_pawnController, handle) || handle == 0xFFFFFFFFU || handle == 0xFFFFFFFEU)
    {
        return nullptr;
    }

    void* chunks = nullptr;

    if (!TryReadMemory(static_cast<const void*>(g_entityIdentityChunks), 0, static_cast<void*>(&chunks), sizeof(chunks)) || !chunks)
    {
        return nullptr;
    }

    const uint32_t entityIndex = handle & 0x7FFFU;

    void* chunk = nullptr;

    if (!TryReadMemory(chunks, static_cast<int>((entityIndex >> 9) * sizeof(void*)), static_cast<void*>(&chunk), sizeof(chunk)) || !chunk)
    {
        return nullptr;
    }

    constexpr int kIdentitySize = 0x70;

    auto* identity = reinterpret_cast<uint8_t*>(chunk) + (static_cast<size_t>(entityIndex & 0x1FFU) * kIdentitySize);

    uint32_t liveHandle = 0;
    void* controller = nullptr;

    if (!SafeRead(identity, 0x10, liveHandle) || liveHandle != handle || !SafeRead(identity, 0x00, controller))
    {
        return nullptr;
    }

    return controller;
}

#endif // _WIN32

// -----------------------------------------------------------------------------
// Calls SetEyeAngles while temporarily bypassing the fake-client early-out.
// -----------------------------------------------------------------------------

bool ApplyReplayEyeAnglesInternal(void* pawn, float pitch, float yaw)
{
    if (!pawn || !g_hookSetEyeAngles.Active())
    {
        return false;
    }

    float angle[3] = { pitch, NormalizeDeg(yaw), 0.0F };

#ifdef _WIN32

    void* controller = ReplayControllerForPawn(pawn);

    uint32_t controllerFlags = 0;

    bool restoreFakeClient = false;

    if (controller && SafeRead(controller, tg::g_entFlags, controllerFlags) && (controllerFlags & 0x100U) != 0)
    {
        const uint32_t publishedFlags = controllerFlags & ~0x100U;

        restoreFakeClient = WriteField(controller, tg::g_entFlags, publishedFlags);
    }

#endif

    g_hookSetEyeAngles.CallOriginal(pawn, angle);

#ifdef _WIN32

    if (restoreFakeClient)
    {
        WriteField(controller, tg::g_entFlags, controllerFlags);
    }

#endif

    return true;
}

// -----------------------------------------------------------------------------
// CCSBot::Update
//
// Skip the bot tick under All lock OR while replaying.
// -----------------------------------------------------------------------------

KHook::Return<void> HookedUpdate(void* bot) noexcept
{
    const int slot = CCSBotToSlot(bot);

    if (slot >= 0 && slot < 64)
    {
        std::scoped_lock lk(g_slotToBotMu);

        g_slotToBot[slot] = bot;
    }

    if (slot >= 0 && (bot_controller_state::GetAll(slot) || motion_recorder::IsReplaying(slot)))
    {
        const uint8_t ticked = 1;

        WriteField(bot, tg::g_botAiTickedFlag, ticked);

        return { KHook::Action::Supersede };
    }

    return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
// CCSBot::Upkeep
//
// Skip the per-frame view tick under All or Aim lock.
//
// During replay the native bot upkeep is also suppressed.
// -----------------------------------------------------------------------------

KHook::Return<void> HookedUpkeep(void* bot) noexcept
{
    const int slot = CCSBotContextToSlot(bot);

    if (slot >= 0 && motion_recorder::IsReplaying(slot))
    {
        return { KHook::Action::Supersede };
    }

    if (slot >= 0 && (bot_controller_state::GetAll(slot) || bot_controller_state::GetAim(slot)))
    {
        return { KHook::Action::Supersede };
    }

    return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
// CCSBot::UpdateLookAngles
// -----------------------------------------------------------------------------

KHook::Return<void> HookedUpdateLookAngles(void* bot) noexcept
{
    const int slot = CCSBotContextToSlot(bot);

    if (slot >= 0 && (motion_recorder::IsReplaying(slot) || bot_controller_state::GetAll(slot) || bot_controller_state::GetAim(slot)))
    {
        return { KHook::Action::Supersede };
    }

    return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
// CCSPlayerPawn::SetEyeAngles
// -----------------------------------------------------------------------------

KHook::Return<void> HookedSetEyeAngles(void* pawn, float* /*angle*/) noexcept
{
    const int slot = pawn ? ControllerSlotForPawn(pawn) : -1;

    if (slot >= 0 && motion_recorder::IsReplaying(slot))
    {
        return { KHook::Action::Supersede };
    }

    return { KHook::Action::Ignore };
}

} // namespace

// -----------------------------------------------------------------------------
// Install
//
// IMPORTANT:
// This function is intentionally idempotent.
//
// Calling Install() while already installed MUST NOT attempt to install a
// second NativeHook. NativeHook::Install() rejects that, and the old code could
// consequently remove an already-working hook set during its failure cleanup.
// -----------------------------------------------------------------------------

bool Install(const nlohmann::json& gd, const sig::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen)
{
    // Critical lifecycle guard.
    //
    // Enable(); Enable(); must leave exactly one hook set installed.
    if (g_installed)
    {
        g_status = "ok";
        return true;
    }

    // Defensive cleanup.
    //
    // Normally there should be nothing installed when g_installed == false,
    // but this also cleans up any partial state left by a previous failed or
    // interrupted installation.
    Remove();

    // ---------------------------------------------------------------------
    // Resolve CCSBot::Update
    // ---------------------------------------------------------------------

    g_addrUpdate = sig::ResolveSig(gd, serverModule, "CCSBot::Update", errorOut, errorOutLen);

    if (!g_addrUpdate)
    {
        g_status = "failed: Update sig";

        return false;
    }

    // ---------------------------------------------------------------------
    // Resolve CCSBot::Upkeep
    // ---------------------------------------------------------------------

    g_addrUpkeep = sig::ResolveSig(gd, serverModule, "CCSBot::Upkeep", errorOut, errorOutLen);

    if (!g_addrUpkeep)
    {
        g_status = "failed: Upkeep sig";

        return false;
    }

    // ---------------------------------------------------------------------
    // Resolve optional CCSBot::UpdateLookAngles
    // ---------------------------------------------------------------------

    char ulaErr[256] = { 0 };

    g_addrUpdateLookAngles = sig::ResolveSig(gd, serverModule, "CCSBot::UpdateLookAngles", ulaErr, sizeof(ulaErr));

    if (!g_addrUpdateLookAngles)
    {
        Warning("[BotController] CCSBot::UpdateLookAngles sig not resolved "
                "(%s); replay view-drive disabled\n",
                ulaErr);
    }

    // ---------------------------------------------------------------------
    // Resolve optional CCSPlayerPawn::SetEyeAngles
    // ---------------------------------------------------------------------

    char seaErr[256] = { 0 };

    g_addrSetEyeAngles = sig::ResolveSig(gd, serverModule, "CCSPlayerPawn::SetEyeAngles", seaErr, sizeof(seaErr));

    if (!g_addrSetEyeAngles)
    {
        Warning("[BotController] CCSPlayerPawn::SetEyeAngles sig not resolved "
                "(%s); replay 1:1 view disabled\n",
                seaErr);
    }

#ifdef _WIN32
    else
    {
        ResolveSetEyeAnglesEntityChunks(g_addrSetEyeAngles);
    }
#endif

    // ---------------------------------------------------------------------
    // Required: CCSBot::Update
    // ---------------------------------------------------------------------

    if (!g_hookUpdate.Install(g_addrUpdate, &HookedUpdate))
    {
        if (errorOut && errorOutLen > 0)
        {
            std::snprintf(errorOut, errorOutLen, "hook CCSBot::Update failed");
        }

        g_status = "failed: hook Update";

        // Full defensive rollback.
        Remove();

        return false;
    }

    // ---------------------------------------------------------------------
    // Required: CCSBot::Upkeep
    // ---------------------------------------------------------------------

    if (!g_hookUpkeep.Install(g_addrUpkeep, &HookedUpkeep))
    {
        if (errorOut && errorOutLen > 0)
        {
            std::snprintf(errorOut, errorOutLen, "hook CCSBot::Upkeep failed");
        }

        g_status = "failed: hook Upkeep";

        // Remove Update as well.
        Remove();

        return false;
    }

    // ---------------------------------------------------------------------
    // Optional: CCSBot::UpdateLookAngles
    // ---------------------------------------------------------------------

    if (g_addrUpdateLookAngles)
    {
        if (!g_hookUpdateLookAngles.Install(g_addrUpdateLookAngles, &HookedUpdateLookAngles))
        {
            Warning("[BotController] hook UpdateLookAngles failed; "
                    "replay view-drive disabled\n");

            g_hookUpdateLookAngles.Remove();

            g_addrUpdateLookAngles = nullptr;
        }
    }

    // ---------------------------------------------------------------------
    // Optional: CCSPlayerPawn::SetEyeAngles
    // ---------------------------------------------------------------------

    if (g_addrSetEyeAngles)
    {
        if (!g_hookSetEyeAngles.Install(g_addrSetEyeAngles, &HookedSetEyeAngles))
        {
            Warning("[BotController] hook SetEyeAngles failed; "
                    "replay 1:1 view disabled\n");

            g_hookSetEyeAngles.Remove();

            g_addrSetEyeAngles = nullptr;

#ifdef _WIN32
            g_entityIdentityChunks = nullptr;
#endif
        }
    }

    g_installed = true;
    g_status = "ok";

    return true;
}

// -----------------------------------------------------------------------------
// Remove
//
// Idempotent and safe to call regardless of g_installed.
//
// This is important for:
//   Disable(); Disable();
//   rollback after partial Install()
//   MetaMod Pause
//   final plugin Unload
// -----------------------------------------------------------------------------

void Remove()
{
    // NativeHook::Remove() itself is safe when no hook exists.
    //
    // Remove in reverse installation order.
    g_hookSetEyeAngles.Remove();
    g_hookUpdateLookAngles.Remove();
    g_hookUpkeep.Remove();
    g_hookUpdate.Remove();

#ifdef _WIN32
    g_entityIdentityChunks = nullptr;
#endif

    // Cached game object pointers must never survive runtime shutdown.
    ClearSlotBotCache();

    g_installed = false;
    g_status = "not_attempted";
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

const char* Status() { return g_status.c_str(); }

void* UpdateAddress() { return g_addrUpdate; }

void* UpkeepAddress() { return g_addrUpkeep; }

void* UpdateLookAnglesAddress() { return g_addrUpdateLookAngles; }

// -----------------------------------------------------------------------------
// Replay helpers
// -----------------------------------------------------------------------------

bool ApplyReplayEyeAngles(void* pawn, float pitch, float yaw) { return ApplyReplayEyeAnglesInternal(pawn, pitch, yaw); }

// Last CCSBot* seen in Update for this slot.
void* BotForSlot(int slot)
{
    if (slot < 0 || slot >= 64)
    {
        return nullptr;
    }

    std::scoped_lock lk(g_slotToBotMu);

    return g_slotToBot[slot];
}

bool IsLiveBotSlot(int slot)
{
    if (slot < 0 || slot >= 64) return false;

    void* bot = nullptr;

    {
        std::scoped_lock lk(g_slotToBotMu);

        bot = g_slotToBot[slot];
    }

    if (!bot) return false;

    return CCSBotToSlot(bot) == slot;
}

} // namespace bot_controller_hooks
} // namespace cs2bc
