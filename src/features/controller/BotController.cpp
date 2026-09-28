#include "core/gameconfig.h"
#include "core/log.h"
// CCSBot Update/Upkeep detours

#include "BotController.h"
#include "BotControllerState.h"
#include "ccsbot_slot.h"
#include "nlohmann/json.hpp"
#include "core/memory_module.h"
#include "MotionRecorder.h"
#include "InputInjector.h"
#include "PawnBinding.h"
#include "offsets.h"
#include "hooks.h"

#include <tier0/dbg.h>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <string>

namespace tg = cs2bc::offsets;

namespace cs2bc {
namespace bot_controller_hooks {

namespace {
void* g_addrUpdate = nullptr;
void* g_addrUpkeep = nullptr;
void* g_addrUpdateLookAngles = nullptr;
void* g_addrSetEyeAngles = nullptr;
void* g_addrGetEyeAngles = nullptr;
bool g_installed = false;
std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

// slot -> last CCSBot* seen in Update (for profile reads by slot)
void* g_slotToBot[64] = { nullptr };
std::mutex g_slotToBotMu;

hooks::NativeHook<void, void*> g_hookUpdate;
hooks::NativeHook<void, void*> g_hookUpkeep;
hooks::NativeHook<void, void*> g_hookUpdateLookAngles;
hooks::NativeHook<void, void*, float*> g_hookSetEyeAngles;
#ifdef _WIN32
hooks::NativeHook<float*, void*, float*> g_hookGetEyeAngles;
#else
// SysV returns the three-float eye-angle value in XMM0/XMM1.
struct EyeAnglesValue { float pitch, yaw, roll; };
static_assert(sizeof(EyeAnglesValue) == 12);
hooks::NativeHook<EyeAnglesValue, void*> g_hookGetEyeAngles;
#endif

// Normalizes an angle to the engine's expected [-180, 180) range.
float NormalizeDeg(float angle)
{
    angle = std::fmod(angle + 180.0F, 360.0F);
    if (angle < 0.0F) angle += 360.0F;
    return angle - 180.0F;
}

// Limits view overrides to the authoritative pawn owning the replay movement services.
int ReplaySlotForPawn(void* pawn)
{
    void* services = nullptr;
    if (!pawn || !GuardedRead(pawn, tg::g_pawnMovementServices, services) || !services) return -1;
    const int slot = input_injector::pawn_binding::ServicesToSlot(services);
    return motion_recorder::IsReplaying(slot) && input_injector::ResolveReplayPawn(slot, services) == pawn ? slot : -1;
}

// Skip the Bot tick under All lock OR while replaying
KHook::Return<void> HookedUpdate(void* bot) noexcept
{
    int slot = CCSBotToSlot(bot);
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

// Skip the per-frame view tick under All or Aim lock.
// EXCEPTION: while a slot is replaying, drive ONLY the view
KHook::Return<void> HookedUpkeep(void* bot) noexcept
{
    int slot = CCSBotContextToSlot(bot);
    if (slot >= 0 && motion_recorder::IsReplaying(slot)) return { KHook::Action::Supersede };
    if (slot >= 0 && (bot_controller_state::GetAll(slot) || bot_controller_state::GetAim(slot)))
    {
        return { KHook::Action::Supersede };
    }
    return { KHook::Action::Ignore };
}

// view replay
KHook::Return<void> HookedUpdateLookAngles(void* bot) noexcept
{
    int slot = CCSBotContextToSlot(bot);
    if (slot >= 0 && (motion_recorder::IsReplaying(slot) || bot_controller_state::GetAll(slot) || bot_controller_state::GetAim(slot)))
        return { KHook::Action::Supersede };
    return { KHook::Action::Ignore };
}

// Suppresses absolute view corrections only for the current replay pawn.
KHook::Return<void> HookedSetEyeAngles(void* pawn, float*) noexcept
{
    if (!motion_recorder::HasAnyReplay()) return { KHook::Action::Ignore };
    if (ReplaySlotForPawn(pawn) >= 0) return { KHook::Action::Supersede };
    return { KHook::Action::Ignore };
}

// Supplies replay view to the engine's normal camera and network publication paths.
#ifdef _WIN32
KHook::Return<float*> HookedGetEyeAngles(void* pawn, float* out) noexcept
{
    if (!out || !motion_recorder::HasAnyReplay()) return { KHook::Action::Ignore };
    const int slot = ReplaySlotForPawn(pawn);
    MovementSnapshot view{};
    if (slot >= 0 && motion_recorder::ReplaySpectatorView(slot, view))
    {
        const float angles[3] = { view.pitch, NormalizeDeg(view.yaw), 0.0F };
        if (TryWriteMemory(out, 0, angles, sizeof(angles))) return { KHook::Action::Supersede, out };
    }
    return { KHook::Action::Ignore };
}
#else
KHook::Return<EyeAnglesValue> HookedGetEyeAngles(void* pawn) noexcept
{
    if (!motion_recorder::HasAnyReplay()) return { KHook::Action::Ignore };
    const int slot = ReplaySlotForPawn(pawn);
    MovementSnapshot view{};
    if (slot >= 0 && motion_recorder::ReplaySpectatorView(slot, view))
        return { KHook::Action::Supersede, { view.pitch, NormalizeDeg(view.yaw), 0.0F } };
    return { KHook::Action::Ignore };
}
#endif

// Resolve a sig from gamedata against the loaded server.dll.
} // namespace

bool Install(const nlohmann::json& gd, const modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen)
{
    g_addrUpdate = gameconfig::ResolveSig(gd, serverModule, "CCSBot::Update", errorOut, errorOutLen);
    if (!g_addrUpdate)
    {
        g_status = "failed: Update sig";
        return false;
    }

    g_addrUpkeep = gameconfig::ResolveSig(gd, serverModule, "CCSBot::Upkeep", errorOut, errorOutLen);
    if (!g_addrUpkeep)
    {
        g_status = "failed: Upkeep sig";
        return false;
    }

    // UpdateLookAngles is optional
    char ulaErr[256] = { 0 };
    g_addrUpdateLookAngles = gameconfig::ResolveSig(gd, serverModule, "CCSBot::UpdateLookAngles", ulaErr, sizeof(ulaErr));
    if (!g_addrUpdateLookAngles)
    {
        BC_LOG_WARN("CCSBot::UpdateLookAngles sig not resolved (%s); replay view-drive disabled\n", ulaErr);
    }

    // View hooks are optional for bot control, but required to start replay.
    char seaErr[256] = { 0 };
    g_addrSetEyeAngles = gameconfig::ResolveSig(gd, serverModule, "CCSPlayerPawn::SetEyeAngles", seaErr, sizeof(seaErr));
    if (!g_addrSetEyeAngles)
    {
        BC_LOG_WARN("CCSPlayerPawn::SetEyeAngles sig not resolved (%s); replay disabled\n", seaErr);
    }
    char geaErr[256] = { 0 };
    g_addrGetEyeAngles = gameconfig::ResolveSig(gd, serverModule, "CBasePlayerPawn::GetEyeAngles", geaErr, sizeof(geaErr));
    if (!g_addrGetEyeAngles)
    {
        BC_LOG_WARN("CBasePlayerPawn::GetEyeAngles sig not resolved (%s); replay disabled\n", geaErr);
    }

    // required: Update
    if (!g_hookUpdate.Install(g_addrUpdate, &HookedUpdate))
    {
        std::snprintf(errorOut, errorOutLen, "hook CCSBot::Update failed");
        g_hookUpdate.Remove();
        g_status = "failed: hook Update";
        return false;
    }

    // required: Upkeep
    if (!g_hookUpkeep.Install(g_addrUpkeep, &HookedUpkeep))
    {
        std::snprintf(errorOut, errorOutLen, "hook CCSBot::Upkeep failed");
        g_hookUpkeep.Remove();
        g_hookUpdate.Remove();
        g_status = "failed: hook Upkeep";
        return false;
    }

    // optional: UpdateLookAngles
    if (g_addrUpdateLookAngles)
    {
        if (!g_hookUpdateLookAngles.Install(g_addrUpdateLookAngles, &HookedUpdateLookAngles))
        {
            BC_LOG_WARN("hook UpdateLookAngles failed; replay view-drive disabled\n");
            g_hookUpdateLookAngles.Remove();
            g_addrUpdateLookAngles = nullptr;
        }
    }

    // optional: SetEyeAngles
    if (g_addrSetEyeAngles)
    {
        if (!g_hookSetEyeAngles.Install(g_addrSetEyeAngles, &HookedSetEyeAngles))
        {
            BC_LOG_WARN("hook SetEyeAngles failed; replay disabled\n");
            g_hookSetEyeAngles.Remove();
            g_addrSetEyeAngles = nullptr;
        }
    }

    // The getter lets the engine publish eye angles and mark its network state dirty.
    if (g_addrGetEyeAngles)
    {
        if (!g_hookGetEyeAngles.Install(g_addrGetEyeAngles, &HookedGetEyeAngles))
        {
            BC_LOG_WARN("hook GetEyeAngles failed; replay disabled\n");
            g_hookGetEyeAngles.Remove();
            g_addrGetEyeAngles = nullptr;
        }
    }

    g_installed = true;
    g_status = "ok";
    return true;
}

void Remove()
{
    if (!g_installed) return;
    g_hookGetEyeAngles.Remove();
    g_hookSetEyeAngles.Remove();
    g_hookUpdateLookAngles.Remove();
    g_hookUpkeep.Remove();
    g_hookUpdate.Remove();
    g_installed = false;
    g_status = "not_attempted";
    {
        std::scoped_lock lk(g_slotToBotMu);
        for (auto& i : g_slotToBot)
            i = nullptr;
    }
}

const char* Status() { return g_status.c_str(); }
void* UpdateAddress() { return g_addrUpdate; }
void* UpkeepAddress() { return g_addrUpkeep; }
void* UpdateLookAnglesAddress() { return g_addrUpdateLookAngles; }

// Requires both correction suppression and normal engine view reads for replay.
bool ReplayViewReady() { return g_hookSetEyeAngles.Active() && g_hookGetEyeAngles.Active(); }

// Last CCSBot* seen in Update for this slot
void* BotForSlot(int slot)
{
    if (slot < 0 || slot >= 64) return nullptr;
    std::scoped_lock lk(g_slotToBotMu);
    return g_slotToBot[slot];
}
} // namespace bot_controller_hooks
} // namespace cs2bc
