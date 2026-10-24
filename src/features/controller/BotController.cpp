#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/log.h"
// CCSBot Update/Upkeep detours

#include "BotController.h"
#include "BotControllerState.h"
#include "ccsbot_slot.h"
#include "nlohmann/json.hpp"
#include "core/cs2_sdk/src/core/memory_module.h"
#include "MotionRecorder.h"
#include "offsets.h"
#include "core/cs2_sdk/src/utils/hooks.h"

#include <tier0/dbg.h>
#include <entityhandle.h>
#include <const.h>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

namespace tg = cs2bc::offsets;

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

cs2_sdk::hooks::NativeHook<void, void*> g_hookUpdate;
cs2_sdk::hooks::NativeHook<void, void*> g_hookUpkeep;
cs2_sdk::hooks::NativeHook<void, void*> g_hookUpdateLookAngles;
cs2_sdk::hooks::NativeHook<void, void*, float*> g_hookSetEyeAngles;

// Normalizes an angle to the engine's expected [-180, 180) range.
float NormalizeDeg(float angle)
{
    angle = std::fmod(angle + 180.0F, 360.0F);
    if (angle < 0.0F) angle += 360.0F;
    return angle - 180.0F;
}

#ifdef _WIN32
// Resolves the entity identity chunk pointer referenced by SetEyeAngles.
bool ResolveSetEyeAnglesEntityChunks(void* setEyeAngles)
{
    g_entityIdentityChunks = nullptr;
    if (!setEyeAngles) return false;

    constexpr size_t kSearchBytes = 0x120;
    uint8_t code[kSearchBytes] = {};
    if (!TryReadMemory(setEyeAngles, 0, code, sizeof(code))) return false;

    auto* functionBase = reinterpret_cast<uint8_t*>(setEyeAngles);
    for (size_t i = 0; i + 10 <= kSearchBytes; ++i)
    {
        if (code[i] != 0x4C || code[i + 1] != 0x8B || code[i + 2] != 0x05 || code[i + 7] != 0x4D || code[i + 8] != 0x85 ||
            code[i + 9] != 0xC0)
            continue;

        int32_t relative = 0;
        std::memcpy(&relative, code + i + 3, sizeof(relative));
        g_entityIdentityChunks = reinterpret_cast<void**>(functionBase + i + 7 + relative);
        return true;
    }
    return false;
}

// Resolves the live controller owning a replay pawn through entity chunks.
void* ReplayControllerForPawn(void* pawn)
{
    if (!pawn || !g_entityIdentityChunks) return nullptr;

    uint32_t handle = 0;
    if (!SafeRead(pawn, tg::g_pawnController, handle) || handle == 0U || handle == 0xFFFFFFFFU || handle == 0xFFFFFFFEU)
        return nullptr;

    void* chunks = nullptr;
    if (!TryReadMemory(static_cast<const void*>(g_entityIdentityChunks), 0, static_cast<void*>(&chunks), sizeof(chunks)) || !chunks)
        return nullptr;

    const uint32_t entityIndex = CEntityHandle(handle).GetEntryIndex();
    void* chunk = nullptr;
    if (!TryReadMemory(chunks, static_cast<int>((entityIndex >> 9) * sizeof(void*)), static_cast<void*>(&chunk), sizeof(chunk)) || !chunk)
        return nullptr;

    constexpr int kIdentitySize = 0x70;
    auto* identity = reinterpret_cast<uint8_t*>(chunk) + (static_cast<size_t>(entityIndex & 0x1FFU) * kIdentitySize);
    uint32_t liveHandle = 0;
    void* controller = nullptr;
    if (!SafeRead(identity, 0x10, liveHandle) || liveHandle != handle || !SafeRead(identity, 0x00, controller)) return nullptr;
    return controller;
}
#endif

// Updates replay angles without queuing a server correction over the next UserCmd.
bool ApplyReplayEyeAnglesInternal(void* pawn, float pitch, float yaw)
{
    if (!pawn || !ReplayViewReady()) return false;

    float angle[3] = { pitch, NormalizeDeg(yaw), 0.0F };
#ifdef _WIN32
    void* controller = ReplayControllerForPawn(pawn);
    uint32_t controllerFlags = 0;
    if (!controller || !SafeRead(controller, tg::g_entFlags, controllerFlags)) return false;
    const bool restoreControllerFlags = (controllerFlags & FL_FAKECLIENT) == 0;
    if (restoreControllerFlags)
    {
        const uint32_t replayFlags = controllerFlags | FL_FAKECLIENT;
        if (!WriteField(controller, tg::g_entFlags, replayFlags)) return false;
    }
#endif
    g_hookSetEyeAngles.CallOriginal(pawn, angle);
#ifdef _WIN32
    if (restoreControllerFlags && !WriteField(controller, tg::g_entFlags, controllerFlags)) return false;
#endif
    return true;
}

// Skip the Bot tick under All lock OR while replaying
KHook::Return<void> HookedUpdate(void* bot) noexcept
{
    if (!bot_controller_state::HasAnyLock() && !motion_recorder::HasAnyReplay()) return { KHook::Action::Ignore };
    int slot = CCSBotToSlot(bot);
    if (slot >= 0 && (bot_controller_state::GetAll(slot) || motion_recorder::IsReplaying(slot)))
    {
        const uint8_t ticked = 1;
        WriteField(bot, tg::g_botAiTickedFlag, ticked);
        return { KHook::Action::Supersede };
    }
    return { KHook::Action::Ignore };
}

// Blocks native view updates owned by a lock or replay.
KHook::Return<void> HookedViewUpdate(void* bot) noexcept
{
    if (!bot_controller_state::HasAnyLock() && !motion_recorder::HasAnyReplay()) return { KHook::Action::Ignore };
    int slot = CCSBotContextToSlot(bot);
    if (slot >= 0 && (motion_recorder::IsReplaying(slot) || bot_controller_state::GetAll(slot) || bot_controller_state::GetAim(slot)))
        return { KHook::Action::Supersede };
    return { KHook::Action::Ignore };
}

// Engine eye-angle
KHook::Return<void> HookedSetEyeAngles(void* pawn, float* angle) noexcept
{
    if (!motion_recorder::HasAnyReplay()) return { KHook::Action::Ignore };
    int slot = pawn ? ControllerSlotForPawn(pawn) : -1;
    if (slot >= 0 && motion_recorder::IsReplaying(slot)) return { KHook::Action::Supersede };
    return { KHook::Action::Ignore };
}
} // namespace

// Installs AI hooks even when replay view support is unavailable.
bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen)
{
    g_addrUpdate = cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "CCSBot::Update", errorOut, errorOutLen);
    if (!g_addrUpdate)
    {
        g_status = "failed: Update sig";
        return false;
    }

    g_addrUpkeep = cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "CCSBot::Upkeep", errorOut, errorOutLen);
    if (!g_addrUpkeep)
    {
        g_status = "failed: Upkeep sig";
        return false;
    }

    // UpdateLookAngles is optional
    char ulaErr[256] = { 0 };
    g_addrUpdateLookAngles = cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "CCSBot::UpdateLookAngles", ulaErr, sizeof(ulaErr));
    if (!g_addrUpdateLookAngles)
    {
        BC_LOG_WARN("UpdateLookAngles unresolved (%s)", ulaErr);
    }

    // Optional for locks and recording, required for replay.
    char seaErr[256] = { 0 };
    g_addrSetEyeAngles = cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "CCSPlayerPawn::SetEyeAngles", seaErr, sizeof(seaErr));
    if (!g_addrSetEyeAngles)
    {
        BC_LOG_WARN("replay disabled: SetEyeAngles unresolved (%s)", seaErr);
    }
#ifdef _WIN32
    else if (!ResolveSetEyeAnglesEntityChunks(g_addrSetEyeAngles))
    {
        BC_LOG_WARN("replay disabled: entity table unresolved");
        g_addrSetEyeAngles = nullptr;
    }
#endif

    // required: Update
    if (!g_hookUpdate.Install(g_addrUpdate, &HookedUpdate))
    {
        std::snprintf(errorOut, errorOutLen, "hook CCSBot::Update failed");
        g_hookUpdate.Remove();
        g_status = "failed: hook Update";
        return false;
    }

    // required: Upkeep
    if (!g_hookUpkeep.Install(g_addrUpkeep, &HookedViewUpdate))
    {
        std::snprintf(errorOut, errorOutLen, "hook CCSBot::Upkeep failed");
        g_hookUpkeep.Remove();
        g_hookUpdate.Remove();
        g_status = "failed: hook Upkeep";
        return false;
    }

    // optional: UpdateLookAngles
    if (g_addrUpdateLookAngles && !g_hookUpdateLookAngles.Install(g_addrUpdateLookAngles, &HookedViewUpdate))
    {
        BC_LOG_WARN("UpdateLookAngles hook failed");
        g_hookUpdateLookAngles.Remove();
        g_addrUpdateLookAngles = nullptr;
    }

    // optional: SetEyeAngles
    if (g_addrSetEyeAngles && !g_hookSetEyeAngles.Install(g_addrSetEyeAngles, &HookedSetEyeAngles))
    {
        BC_LOG_WARN("replay disabled: SetEyeAngles hook failed");
        g_hookSetEyeAngles.Remove();
        g_addrSetEyeAngles = nullptr;
    }

    g_installed = true;
    g_status = ReplayViewReady() ? "ok" : "degraded: replay view unavailable";
    return true;
}

void Remove()
{
    if (!g_installed) return;
    g_hookSetEyeAngles.Remove();
#ifdef _WIN32
    g_entityIdentityChunks = nullptr;
#endif
    g_hookUpdateLookAngles.Remove();
    g_hookUpkeep.Remove();
    g_hookUpdate.Remove();
    g_installed = false;
    g_status = "not_attempted";
}

const char* Status() { return g_status.c_str(); }
void* UpdateAddress() { return g_addrUpdate; }
void* UpkeepAddress() { return g_addrUpkeep; }
void* UpdateLookAnglesAddress() { return g_addrUpdateLookAngles; }

// Requires both the native hook and its platform-specific dependencies.
bool ReplayViewReady()
{
#ifdef _WIN32
    if (!g_entityIdentityChunks) return false;
#endif
    return g_hookSetEyeAngles.Active();
}

// Stops only the replay whose eye-angle update failed.
bool ApplyReplayEyeAngles(int slot, void* pawn, float pitch, float yaw)
{
    if (!motion_recorder::IsReplaying(slot)) return false;
    const uint64_t generation = motion_recorder::PawnGeneration(slot);
    const bool applied = ApplyReplayEyeAnglesInternal(pawn, pitch, yaw);
    if (generation != motion_recorder::PawnGeneration(slot)) return false;
    if (!applied)
    {
        BC_LOG_WARN("slot %d: view failed; replay stopped", slot);
        motion_recorder::StopReplay(slot);
        return false;
    }
    return motion_recorder::IsReplaying(slot);
}
} // namespace bot_controller_hooks
} // namespace cs2bc
