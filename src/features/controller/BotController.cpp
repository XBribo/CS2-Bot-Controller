#include "core/gameconfig.h"
#include "core/log.h"
// CCSBot Update/Upkeep detours

#include "BotController.h"
#include "BotControllerState.h"
#include "WeaponLocker.h"
#include "WeaponLockerState.h"
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
struct BotPawnOwner
{
    void* bot = nullptr;
    uint32_t pawnHandle = 0;
};
BotPawnOwner g_bestWeaponRequests[64]{};
struct PerceptionSnapshot
{
    BotPawnOwner owner{};
    NativePerceptionState state{};
};
PerceptionSnapshot g_perception[64]{};
BotPawnOwner g_replayNavigation[64]{};
uint32_t g_perceptionSerial = 0;
bool g_nativePerceptionReady = false;
bool g_replayNativeFovOverride = false;
#ifdef _WIN32
using InvalidatePathFn = void(BC_FASTCALL*)(void*);
#else
// Linux inlines InvalidatePath; the equivalent path-state exit takes an unused state and the bot.
using InvalidatePathFn = void(BC_FASTCALL*)(void*, void*);
#endif
InvalidatePathFn g_invalidatePath = nullptr;
using VisiblePosFn = bool(BC_FASTCALL*)(void*, const void*, bool, void*);
using VisiblePlayerFn = bool(BC_FASTCALL*)(void*, void*, bool, uint8_t*);

hooks::NativeHook<void, void*> g_hookUpdate;
hooks::NativeHook<void, void*> g_hookUpkeep;
hooks::NativeHook<void, void*> g_hookUpdateLookAngles;
hooks::NativeHook<void, void*, float*> g_hookSetEyeAngles;
hooks::NativeHook<bool, void*, const void*, bool, void*> g_hookIsVisiblePos;
hooks::NativeHook<bool, void*, void*, bool, uint8_t*> g_hookIsVisiblePlayer;
hooks::NativeHook<bool, void*> g_hookLadderUpdate;
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

// Rejects human takeover and identifies the current AI-owned pawn by its serial-bearing handle.
bool ReadAiOwner(int slot, void* bot, uint32_t& pawnHandle)
{
    if (slot < 0 || slot >= 64) return false;
    const auto owner = ResolveSlot(bot);
    if (owner.slot != slot || !owner.pawn) return false;
    const auto controllers = ReadPawnControllerHandles(owner.pawn);
    if (controllers.controllerSlot != slot || controllers.originalControllerIndex < 1 ||
        controllers.controllerHandle != controllers.originalControllerHandle) return false;
    void* identity = nullptr;
    return GuardedRead(owner.pawn, tg::g_entIdentity, identity) && identity &&
        GuardedRead(identity, tg::g_entIdentityEHandle, pawnHandle) &&
        pawnHandle != 0 && pawnHandle != UINT32_MAX;
}

// Limits queued selection to an unlocked AI owner and its current pawn instance.
bool ReadBestWeaponOwner(int slot, void* bot, uint32_t& pawnHandle)
{
    return g_installed && slot >= 0 && slot < 64 && weapon_locker_hooks::WeaponHooksReady() &&
        !bot_controller_state::GetAll(slot) && !motion_recorder::IsReplaying(slot) &&
        weapon_locker_state::Get(slot) == LockTarget::None && ReadAiOwner(slot, bot, pawnHandle);
}

// Limits view overrides to the authoritative pawn owning the replay movement services.
int ReplaySlotForPawn(void* pawn)
{
    void* services = nullptr;
    if (!pawn || !GuardedRead(pawn, tg::g_pawnMovementServices, services) || !services) return -1;
    const int slot = input_injector::pawn_binding::ServicesToSlot(services);
    return motion_recorder::IsReplaying(slot) && input_injector::ResolveReplayPawn(slot, services) == pawn ? slot : -1;
}

// Requires the authoritative replay pawn as well as a live AI owner for vision/navigation hooks.
int NativeReplaySlot(void* bot, uint32_t& pawnHandle)
{
    if (!NativePerceptionReady()) return -1;
    const auto owner = ResolveSlot(bot);
    return ReadAiOwner(owner.slot, bot, pawnHandle) && ReplaySlotForPawn(owner.pawn) == owner.slot ? owner.slot : -1;
}

// Captures actual AI output, never a fabricated fresh update for a paused or recycled owner.
void CaptureNativePerception(void* bot, int slot)
{
    uint32_t pawnHandle = 0;
    if (!NativePerceptionReady() || !ReadAiOwner(slot, bot, pawnHandle)) return;
    NativePerceptionState state{};
    uint8_t visible = 0, visibleParts = 0, lastDead = 0;
    const bool valid = SafeRead(bot, tg::g_botEnemy, state.enemyHandle) &&
        SafeRead(bot, tg::g_botEnemyVisible, visible) &&
        SafeRead(bot, tg::g_botVisibleEnemyParts, visibleParts) &&
        SafeRead(bot, tg::g_botNearbyEnemyCount, state.nearbyEnemyCount) &&
        SafeRead(bot, tg::g_botLastEnemyDead, lastDead) &&
        SafeRead(bot, tg::g_botLastSawEnemyTimestamp, state.lastSawEnemyTimestamp) &&
        SafeRead(bot, tg::g_botFirstSawEnemyTimestamp, state.firstSawEnemyTimestamp) &&
        SafeRead(bot, tg::g_botCurrentEnemyAcquireTimestamp, state.currentEnemyAcquireTimestamp);
    state.valid = valid ? 1 : 0;
    state.hasEnemy = valid && state.enemyHandle != 0 && state.enemyHandle != UINT32_MAX &&
        state.enemyHandle != UINT32_MAX - 1 ? 1 : 0;
    state.enemyVisible = visible != 0 ? 1 : 0;
    state.visibleEnemyParts = visibleParts;
    state.lastEnemyDead = lastDead != 0 ? 1 : 0;
    std::scoped_lock lk(g_slotToBotMu);
    state.updateSerial = ++g_perceptionSerial;
    g_perception[slot] = { { bot, pawnHandle }, state };
}

// Retires only navigation suppressed for this pawn, before its first post-replay AI Update.
void ReleaseReplayNavigation(int slot, void* bot)
{
    BotPawnOwner suppressed{};
    {
        std::scoped_lock lk(g_slotToBotMu);
        suppressed = g_replayNavigation[slot];
        g_replayNavigation[slot] = {};
    }
    uint32_t pawnHandle = 0;
    if (suppressed.bot == bot && g_invalidatePath && ReadAiOwner(slot, bot, pawnHandle) &&
        suppressed.pawnHandle == pawnHandle)
    {
#ifdef _WIN32
        g_invalidatePath(bot);
#else
        g_invalidatePath(nullptr, bot);
#endif
    }
}

// All remains a full-brain lock; safe replay keeps AI perception/decision state warm beneath input injection.
KHook::Return<void> HookedUpdate(void* bot) noexcept
{
    int slot = CCSBotToSlot(bot);
    if (slot >= 0 && slot < 64)
    {
        std::scoped_lock lk(g_slotToBotMu);
        g_slotToBot[slot] = bot;
    }
    if (slot >= 0 && (bot_controller_state::GetAll(slot) ||
        (motion_recorder::IsReplaying(slot) && !NativePerceptionReady())))
    {
        const uint8_t ticked = 1;
        WriteField(bot, tg::g_botAiTickedFlag, ticked);
        return { KHook::Action::Supersede };
    }
    if (slot >= 0 && !motion_recorder::IsReplaying(slot)) ReleaseReplayNavigation(slot, bot);
    return { KHook::Action::Ignore };
}

// Consumes one request after AI Update has refreshed combat state; stale owners are discarded.
KHook::Return<void> UpdatePost(void* bot) noexcept
{
    const int slot = CCSBotToSlot(bot);
    if (slot < 0 || slot >= 64) return { KHook::Action::Ignore };
    const bool updated = !KHook::WasOriginalFunctionSkipped();
    if (updated) CaptureNativePerception(bot, slot);
    BotPawnOwner request{};
    {
        std::scoped_lock lk(g_slotToBotMu);
        request = g_bestWeaponRequests[slot];
        g_bestWeaponRequests[slot] = {};
    }
    uint32_t pawnHandle = 0;
    if (updated && request.bot == bot && ReadBestWeaponOwner(slot, bot, pawnHandle) && request.pawnHandle == pawnHandle)
        weapon_locker_hooks::EquipBestWeaponRaw(bot, true);
    return { KHook::Action::Ignore };
}

// Keeps non-view Upkeep warm for safe replay; All/Aim remain explicit full/view locks.
KHook::Return<void> HookedUpkeep(void* bot) noexcept
{
    int slot = CCSBotContextToSlot(bot);
    if (slot >= 0 && motion_recorder::IsReplaying(slot) && !NativePerceptionReady()) return { KHook::Action::Supersede };
    if (slot >= 0 && (bot_controller_state::GetAll(slot) || bot_controller_state::GetAim(slot)))
    {
        return { KHook::Action::Supersede };
    }
    return { KHook::Action::Ignore };
}

// Disables only replay's FOV cone; KHook recall preserves native LOS/smoke checks and other hooks.
KHook::Return<bool> IsVisiblePosPre(void* bot, const void* position, bool testFov, void* context) noexcept
{
    uint32_t pawnHandle = 0;
    if (testFov && g_replayNativeFovOverride && NativeReplaySlot(bot, pawnHandle) >= 0)
        return KHook::Recall(static_cast<VisiblePosFn>(nullptr), KHook::Return<bool>{ KHook::Action::Ignore },
            bot, position, false, context);
    return { KHook::Action::Ignore };
}

// Uses the same replay-only FOV policy for the player visibility overload.
KHook::Return<bool> IsVisiblePlayerPre(void* bot, void* player, bool testFov, uint8_t* visibleParts) noexcept
{
    uint32_t pawnHandle = 0;
    if (testFov && g_replayNativeFovOverride && NativeReplaySlot(bot, pawnHandle) >= 0)
        return KHook::Recall(static_cast<VisiblePlayerFn>(nullptr), KHook::Return<bool>{ KHook::Action::Ignore },
            bot, player, false, visibleParts);
    return { KHook::Action::Ignore };
}

// Blocks the AI ladder navigation Teleport, not the player's native ladder physics.
KHook::Return<bool> LadderUpdatePre(void* ladderState) noexcept
{
    void* bot = nullptr;
    uint32_t pawnHandle = 0;
    if (GuardedRead(ladderState, 0, bot) && bot)
    {
        const int slot = NativeReplaySlot(bot, pawnHandle);
        if (slot >= 0)
        {
            std::scoped_lock lk(g_slotToBotMu);
            g_replayNavigation[slot] = { bot, pawnHandle };
            return { KHook::Action::Supersede, true };
        }
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
    if (!g_hookUpdate.Install(g_addrUpdate, &HookedUpdate, &UpdatePost))
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

    // Optional warm replay requires every output-ownership hook, not just readable fields.
    char perceptionError[256]{};
    void* visiblePos = gameconfig::ResolveSig(gd, serverModule, "CCSBot::IsVisiblePos", perceptionError, sizeof(perceptionError));
    if (visiblePos) g_hookIsVisiblePos.Install(visiblePos, &IsVisiblePosPre);
    void* visiblePlayer = gameconfig::ResolveSig(gd, serverModule, "CCSBot::IsVisiblePlayer", perceptionError, sizeof(perceptionError));
    if (visiblePlayer) g_hookIsVisiblePlayer.Install(visiblePlayer, &IsVisiblePlayerPre);
    void* ladderUpdate = gameconfig::ResolveSig(gd, serverModule, "CCSBot::LadderStateUpdate", perceptionError, sizeof(perceptionError));
    if (ladderUpdate) g_hookLadderUpdate.Install(ladderUpdate, &LadderUpdatePre);
    g_invalidatePath = reinterpret_cast<InvalidatePathFn>(gameconfig::ResolveSig(
        gd, serverModule, "CCSBot::InvalidatePath", perceptionError, sizeof(perceptionError)));
    const bool haveFields = tg::g_botEnemy >= 0 && tg::g_botEnemyVisible >= 0 && tg::g_botVisibleEnemyParts >= 0 &&
        tg::g_botNearbyEnemyCount >= 0 && tg::g_botLastEnemyDead >= 0 && tg::g_botLastSawEnemyTimestamp >= 0 &&
        tg::g_botFirstSawEnemyTimestamp >= 0 && tg::g_botCurrentEnemyAcquireTimestamp >= 0;
    g_nativePerceptionReady = haveFields && g_hookIsVisiblePos.Active() && g_hookIsVisiblePlayer.Active() &&
        g_hookLadderUpdate.Active() && g_invalidatePath && g_hookUpdateLookAngles.Active() && ReplayViewReady();
    if (!g_nativePerceptionReady)
        BC_LOG_WARN("Native replay perception disabled: fields=%d vision=%d/%d ladder=%d path=%d view=%d; "
            "replay AI remains paused\n", haveFields, g_hookIsVisiblePos.Active(), g_hookIsVisiblePlayer.Active(),
            g_hookLadderUpdate.Active(), g_invalidatePath != nullptr, g_hookUpdateLookAngles.Active() && ReplayViewReady());
    g_installed = true;
    g_status = "ok";
    return true;
}

void Remove()
{
    if (!g_installed) return;
    g_nativePerceptionReady = false;
    g_hookLadderUpdate.Remove();
    g_hookIsVisiblePlayer.Remove();
    g_hookIsVisiblePos.Remove();
    g_hookGetEyeAngles.Remove();
    g_hookSetEyeAngles.Remove();
    g_hookUpdateLookAngles.Remove();
    g_hookUpkeep.Remove();
    g_hookUpdate.Remove();
    for (int slot = 0; slot < 64; ++slot) ReleaseReplayNavigation(slot, BotForSlot(slot));
    g_invalidatePath = nullptr;
    g_replayNativeFovOverride = false;
    g_installed = false;
    g_status = "not_attempted";
    {
        std::scoped_lock lk(g_slotToBotMu);
        for (auto& i : g_slotToBot)
            i = nullptr;
        for (auto& request : g_bestWeaponRequests)
            request = {};
        for (auto& snapshot : g_perception)
            snapshot = {};
        g_perceptionSerial = 0;
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

// Accepts one best-weapon selection for the next native AI update, not an immediate switch.
bool RequestEquipBestWeapon(int slot)
{
    void* bot = BotForSlot(slot);
    uint32_t pawnHandle = 0;
    if (!ReadBestWeaponOwner(slot, bot, pawnHandle)) return false;
    std::scoped_lock lk(g_slotToBotMu);
    if (g_slotToBot[slot] != bot) return false;
    g_bestWeaponRequests[slot] = { bot, pawnHandle };
    return true;
}

// Reports actual runtime readiness so consumers can retain managed detection when hooks are unavailable.
bool NativePerceptionReady() { return g_installed && g_nativePerceptionReady && weapon_locker_hooks::WeaponHooksReady(); }

// Rejects stale snapshots after pawn replacement or human takeover without mutating engine state.
bool GetNativePerceptionState(int slot, NativePerceptionState& out)
{
    out = {};
    if (!NativePerceptionReady() || slot < 0 || slot >= 64) return false;
    PerceptionSnapshot snapshot{};
    {
        std::scoped_lock lk(g_slotToBotMu);
        snapshot = g_perception[slot];
        if (snapshot.owner.bot != g_slotToBot[slot]) return false;
    }
    uint32_t pawnHandle = 0;
    if (!snapshot.state.valid || !ReadAiOwner(slot, snapshot.owner.bot, pawnHandle) ||
        snapshot.owner.pawnHandle != pawnHandle) return false;
    out = snapshot.state;
    return true;
}

// The caller owns the replay FOV policy; native visibility still evaluates LOS and smoke.
bool SetReplayNativeFovOverride(bool enabled)
{
    if (!NativePerceptionReady()) return false;
    g_replayNativeFovOverride = enabled;
    return true;
}
} // namespace bot_controller_hooks
} // namespace cs2bc
