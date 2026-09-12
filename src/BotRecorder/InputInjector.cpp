// CS2 movement hooks
// ProcessMovement (record + apply pre)
// FinishMove (replay post into MoveData + commit)
// PlayerRunCommand(subtick record + re-inject)

#include "networkbasetypes.pb.h"
#include "nlohmann/json.hpp" // NOLINT(misc-include-cleaner)
#include "playercommand.h"

#include "InputInjector.h"
#include "BotController.h"
#include "ccsbot_slot.h"
#include "sig_scan.h"
#include "MotionRecorder.h"
#include "ProjectileBirthAlign.h"
#include "usercmd.pb.h"
#include "version_targets.h"
#include "hooks.h"

#include <algorithm> // NOLINT(misc-include-cleaner)
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector> // NOLINT(misc-include-cleaner)

#include <tier0/dbg.h>

namespace tg = cs2bc::targets;

namespace cs2bc {
namespace input_injector {

namespace {
constexpr float kUsercmdKeyboardMoveScale = 450.0F;
constexpr uint64_t kInForward = 1ULL << 3;
constexpr uint64_t kInBack = 1ULL << 4;
constexpr uint64_t kInMoveLeft = 1ULL << 9;
constexpr uint64_t kInMoveRight = 1ULL << 10;
constexpr uint64_t kMovementButtonMask = kInForward | kInBack | kInMoveLeft | kInMoveRight;

void* g_addrProcessMovement = nullptr;
void* g_addrFinishMove = nullptr;
void* g_addrPlayerRunCommand = nullptr;
void* g_addrPhysicsSimulate = nullptr;

hooks::NativeHook<void, void*, void*> g_hookProcessMovement;
hooks::NativeHook<void, void*, void*, void*> g_hookFinishMove;
hooks::NativeHook<void, void*, void*> g_hookPlayerRunCommand;
hooks::NativeHook<void, void*> g_hookPhysicsSimulate;

struct MovementFrame
{
    int slot;
    void* services;
    void* moveData;
    bool recording;
    bool replaying;
};
thread_local std::vector<MovementFrame> g_processFrames;
thread_local std::vector<MovementFrame> g_finishFrames;
thread_local std::vector<MovementFrame> g_physicsFrames;
bool g_installed = false;
// True once PhysicsSimulate is hooked
bool g_physicsActive = false;
// True once PlayerRunCommand is hooked
bool g_subtickActive = false;
std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

// slot -> live CCSPlayer_MovementServices*
std::array<std::atomic<void*>, kMaxSlots> g_slotServices{};
std::array<std::atomic<void*>, kMaxSlots> g_slotPawns{};

enum class UsercmdInjectionPhase : uint8_t
{
    PendingPress,
    Holding,
    PendingRelease
};

struct UsercmdInjection
{
    int64_t id;
    uint64_t buttonMask;
    int64_t expiresAtMs;
    int durationMs;
    UsercmdInjectionPhase phase;
};

struct UsercmdMovement
{
    int64_t id;
    float forwardMove;
    float leftMove;
};

struct UsercmdSuppression
{
    int64_t id;
    uint64_t buttonMask;
    int64_t expiresAtMs;
    bool persistent;
    bool releasePending;
};

std::array<std::vector<UsercmdInjection>, kMaxSlots> g_usercmdInjections{};
std::array<std::vector<UsercmdSuppression>, kMaxSlots> g_usercmdSuppressions{};
std::array<std::vector<UsercmdMovement>, kMaxSlots> g_usercmdMovements{};
std::array<uint64_t, kMaxSlots> g_injectedHeldMasks{};
std::array<uint64_t, kMaxSlots> g_movementHeldMasks{};
std::mutex g_usercmdInjectionMutex;
std::atomic<int64_t> g_nextUsercmdInjectionId{ 1 };
std::atomic<int64_t> g_nextUsercmdSuppressionId{ 1 };
std::atomic<int64_t> g_nextUsercmdMovementId{ 1 };

std::atomic<uint64_t> g_hookCalls{ 0 };
std::atomic<int> g_lastSlot{ -1 };
std::atomic<uint64_t> g_finishMoveCalls{ 0 };
std::atomic<uint64_t> g_playerRunCommandCalls{ 0 };
std::atomic<uint64_t> g_usercmdMovementApplyCalls{ 0 };
std::atomic<int> g_lastUsercmdMovementSlot{ -1 };
std::atomic<int> g_lastUsercmdForwardMove{ 0 };
std::atomic<int> g_lastUsercmdLeftMove{ 0 };
std::atomic<uint64_t> g_physicsSimulateCalls{ 0 };
std::atomic<int> g_lastPhysicsSlot{ -1 };
std::atomic<uint64_t> g_replayCommitCalls{ 0 };
std::atomic<uint64_t> g_slotResolveCalls{ 0 };
std::atomic<uint64_t> g_slotResolveFailures{ 0 };
std::atomic<uintptr_t> g_lastServices{ 0 };
std::atomic<uintptr_t> g_lastPawn{ 0 };
std::atomic<uint32_t> g_lastControllerHandle{ 0 };
std::atomic<uint32_t> g_lastOriginalControllerHandle{ 0 };
std::atomic<int> g_lastControllerIndex{ -1 };
std::atomic<int> g_lastOriginalControllerIndex{ -1 };
std::atomic<int> g_lastOwnerSlot{ -1 };

bool IsThrowableUtilityDef(int def) { return def >= 43 && def <= 48; }

// Reports whether a slot can index the fixed replay state arrays.
bool ValidSlotIndex(int slot) { return slot >= 0 && slot < kMaxSlots; }

// Reads the helper pawn field embedded in movement services.
void* ServicesToPawnField(void* services)
{
    void* pawn = nullptr;
    return GuardedRead(services, tg::g_servicesPawn, pawn) ? pawn : nullptr;
}

// Verifies that a pawn currently owns the supplied movement services.
bool PawnOwnsServices(void* pawn, void* services)
{
    if (!pawn || !services) return false;
    void* liveServices = nullptr;
    return GuardedRead(pawn, tg::g_pawnMovementServices, liveServices) && liveServices == services;
}

// Registers a readable pawn whose current owner matches the requested slot.
} // namespace

bool SetReplayPawn(int slot, void* pawn)
{
    // Replay ownership is bot-only. Recording may target a human, but replay
    // state and replay pawn registration must never be attached to a human.
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || motion_recorder::IsReplaying(slot))
        return false;

    g_slotPawns[slot].store(nullptr, std::memory_order_release);

    if (!pawn) return false;

    void* identity = nullptr;
    uint32_t handle = 0;

    if (!GuardedRead(pawn, tg::g_entIdentity, identity) || !identity || !GuardedRead(identity, tg::g_entIdentityEHandle, handle) ||
        handle == 0U || handle == 0xFFFFFFFFU)
    {
        return false;
    }

    const int ownerSlot = ControllerSlotForPawn(pawn);
    if (ownerSlot != slot) return false;

    g_slotPawns[slot].store(pawn, std::memory_order_release);
    return true;
}

// Removes a registered pawn before the entity can be recycled.
void ClearReplayPawn(int slot)
{
    if (ValidSlotIndex(slot)) g_slotPawns[slot].store(nullptr, std::memory_order_release);
}

// Returns a registered pawn only when its movement-services link is current.
void* ResolveReplayPawn(int slot, void* services)
{
    if (ValidSlotIndex(slot))
    {
        void* registered = g_slotPawns[slot].load(std::memory_order_acquire);
        if (PawnOwnsServices(registered, services)) return registered;
    }

    void* fieldPawn = ServicesToPawnField(services);
    return PawnOwnsServices(fieldPawn, services) ? fieldPawn : nullptr;
}

// Finds a registered slot by validating every pawn-to-services link.
namespace {

int RegisteredSlotForServices(void* services)
{
    if (!services) return -1;
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        void* pawn = g_slotPawns[slot].load(std::memory_order_acquire);
        if (PawnOwnsServices(pawn, services)) return slot;
    }
    return -1;
}

int ServicesToSlot(void* services)
{
    g_slotResolveCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastServices.store(reinterpret_cast<uintptr_t>(services), std::memory_order_relaxed);
    g_lastPawn.store(0, std::memory_order_relaxed);
    g_lastControllerHandle.store(0, std::memory_order_relaxed);
    g_lastOriginalControllerHandle.store(0, std::memory_order_relaxed);
    g_lastControllerIndex.store(-1, std::memory_order_relaxed);
    g_lastOriginalControllerIndex.store(-1, std::memory_order_relaxed);
    g_lastOwnerSlot.store(-1, std::memory_order_relaxed);

    if (!services)
    {
        g_slotResolveFailures.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    void* pawn = ServicesToPawnField(services);
    if (!PawnOwnsServices(pawn, services)) pawn = nullptr;

    PawnControllerHandles handles = ReadPawnControllerHandles(pawn);
    g_lastPawn.store(reinterpret_cast<uintptr_t>(pawn), std::memory_order_relaxed);
    g_lastControllerHandle.store(handles.controllerHandle, std::memory_order_relaxed);
    g_lastOriginalControllerHandle.store(handles.originalControllerHandle, std::memory_order_relaxed);
    g_lastControllerIndex.store(handles.controllerIndex, std::memory_order_relaxed);
    g_lastOriginalControllerIndex.store(handles.originalControllerIndex, std::memory_order_relaxed);
    int ownerSlot = handles.ownerSlot;
    if (ownerSlot < 0) ownerSlot = RegisteredSlotForServices(services);
    g_lastOwnerSlot.store(ownerSlot, std::memory_order_relaxed);
    if (ownerSlot < 0) g_slotResolveFailures.fetch_add(1, std::memory_order_relaxed);
    return ownerSlot;
}

// services -> pawn -> WeaponServices*, for the recording weapon tap.
void* ServicesToWeaponServices(int slot, void* services)
{
    void* pawn = ResolveReplayPawn(slot, services);
    if (!pawn) return nullptr;
    void* weaponServices = nullptr;
    return GuardedRead(pawn, tg::g_pawnWeaponServices, weaponServices) ? weaponServices : nullptr;
}

float NormalizeDeg(float a)
{
    a = std::fmod(a + 180.0F, 360.0F);
    if (a < 0.0F) a += 360.0F;
    return a - 180.0F;
}

// Returns monotonic time in milliseconds for injection expiry checks
int64_t MonotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Creates an independently cancellable usercmd button injection
} // namespace

int64_t InjectUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || buttonMask == 0 || durationMs < 0 ||
        !g_subtickActive || motion_recorder::IsReplaying(slot))
    {
        return -1;
    }

    const int64_t id = g_nextUsercmdInjectionId.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock lock(g_usercmdInjectionMutex);

    if (!g_installed || !bot_controller_hooks::IsLiveBotSlot(slot) || motion_recorder::IsReplaying(slot)) return -1;

    g_usercmdInjections[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .durationMs = durationMs, .phase = UsercmdInjectionPhase::PendingPress });

    return id;
}

// Creates an independently cancellable persistent analog movement override
int64_t StartUsercmdMovement(int slot, float forwardMove, float leftMove)
{
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || !std::isfinite(forwardMove) ||
        !std::isfinite(leftMove) || !g_subtickActive || motion_recorder::IsReplaying(slot))
    {
        return -1;
    }

    const int64_t id = g_nextUsercmdMovementId.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock lock(g_usercmdInjectionMutex);

    if (!g_installed || !bot_controller_hooks::IsLiveBotSlot(slot) || motion_recorder::IsReplaying(slot)) return -1;

    g_usercmdMovements[slot].push_back(
        { .id = id, .forwardMove = std::clamp(forwardMove, -1.0F, 1.0F), .leftMove = std::clamp(leftMove, -1.0F, 1.0F) });

    return id;
}

// Updates one persistent analog movement override
bool UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove)
{
    if (!g_installed || !g_subtickActive || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || movementId <= 0 ||
        !std::isfinite(forwardMove) || !std::isfinite(leftMove) || motion_recorder::IsReplaying(slot))
    {
        return false;
    }

    std::scoped_lock lock(g_usercmdInjectionMutex);

    for (UsercmdMovement& movement : g_usercmdMovements[slot])
    {
        if (movement.id != movementId) continue;

        movement.forwardMove = std::clamp(forwardMove, -1.0F, 1.0F);
        movement.leftMove = std::clamp(leftMove, -1.0F, 1.0F);
        return true;
    }

    return false;
}

// Cancels one persistent analog movement override
bool CancelUsercmdMovement(int slot, int64_t movementId)
{
    if (!ValidSlotIndex(slot) || movementId <= 0) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    auto& movements = g_usercmdMovements[slot];
    for (auto it = movements.begin(); it != movements.end(); ++it)
    {
        if (it->id != movementId) continue;
        movements.erase(it);
        return true;
    }
    return false;
}

// Cancels one injection without affecting other active tokens
bool CancelUsercmdInjection(int slot, int64_t injectionId)
{
    if (!ValidSlotIndex(slot) || injectionId <= 0) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    auto& injections = g_usercmdInjections[slot];
    for (auto it = injections.begin(); it != injections.end(); ++it)
    {
        if (it->id != injectionId) continue;

        if (it->phase == UsercmdInjectionPhase::PendingPress) injections.erase(it);
        else
            it->phase = UsercmdInjectionPhase::PendingRelease;
        return true;
    }
    return false;
}

// Suppresses selected usercmd buttons until the requested duration expires
bool SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || buttonMask == 0 || durationMs <= 0 ||
        !g_subtickActive || motion_recorder::IsReplaying(slot))
    {
        return false;
    }

    const int64_t expiresAtMs = MonotonicMilliseconds() + durationMs;

    std::scoped_lock lock(g_usercmdInjectionMutex);

    if (!g_installed || !bot_controller_hooks::IsLiveBotSlot(slot) || motion_recorder::IsReplaying(slot)) return false;

    g_usercmdSuppressions[slot].push_back(
        { .id = 0, .buttonMask = buttonMask, .expiresAtMs = expiresAtMs, .persistent = false, .releasePending = true });

    return true;
}

// Creates an independently cancellable persistent usercmd suppression
int64_t StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || buttonMask == 0 || !g_subtickActive ||
        motion_recorder::IsReplaying(slot))
    {
        return -1;
    }

    const int64_t id = g_nextUsercmdSuppressionId.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock lock(g_usercmdInjectionMutex);

    if (!g_installed || !bot_controller_hooks::IsLiveBotSlot(slot) || motion_recorder::IsReplaying(slot)) return -1;

    g_usercmdSuppressions[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .persistent = true, .releasePending = true });

    return id;
}

// Cancels one persistent usercmd suppression by its token
bool CancelUsercmdSuppression(int slot, int64_t suppressionId)
{
    if (!ValidSlotIndex(slot) || suppressionId <= 0) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    auto& suppressions = g_usercmdSuppressions[slot];
    for (auto it = suppressions.begin(); it != suppressions.end(); ++it)
    {
        if (it->id != suppressionId) continue;
        suppressions.erase(it);
        return true;
    }
    return false;
}

// Removes every injection and suppression so replay starts without deferred input
void ClearUsercmdInjections(int slot)
{
    if (!ValidSlotIndex(slot)) return;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    g_usercmdInjections[slot].clear();
    g_usercmdSuppressions[slot].clear();
    g_usercmdMovements[slot].clear();
    g_injectedHeldMasks[slot] = 0;
    g_movementHeldMasks[slot] = 0;
}

// Reports whether a slot has injections waiting for command processing
namespace {

bool HasUsercmdInjection(int slot)
{
    if (!ValidSlotIndex(slot)) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    return !g_usercmdInjections[slot].empty();
}

// Reports whether a slot has button suppressions waiting for command processing
bool HasUsercmdSuppression(int slot)
{
    if (!ValidSlotIndex(slot)) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    return !g_usercmdSuppressions[slot].empty();
}

// Reports whether a slot has an active analog movement override
bool HasUsercmdMovement(int slot)
{
    if (!ValidSlotIndex(slot)) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    return !g_usercmdMovements[slot].empty();
}

// Replaces Bot AI analog movement after the final command is generated
bool ApplyUsercmdMovement(int slot, PlayerCommand* pc, CBaseUserCmdPB* base) // NOLINT(readability-non-const-parameter)
{
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || pc == nullptr || base == nullptr)
        return false;

    UsercmdMovement movement{};
    uint64_t previousMask = 0;

    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        const auto& movements = g_usercmdMovements[slot];
        if (movements.empty()) return false;
        movement = movements.back();
        previousMask = g_movementHeldMasks[slot];
    }

    uint64_t movementMask = 0;
    if (movement.forwardMove > 0.0F) movementMask |= kInForward;
    else if (movement.forwardMove < 0.0F)
        movementMask |= kInBack;

    if (movement.leftMove > 0.0F) movementMask |= kInMoveLeft;
    else if (movement.leftMove < 0.0F)
        movementMask |= kInMoveRight;

    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        g_movementHeldMasks[slot] = movementMask;
    }

    const uint64_t pressedMask = movementMask & ~previousMask;
    const uint64_t releasedMask = previousMask & ~movementMask;
    const uint64_t held = (pc->buttonstates.m_pButtonStates[0] & ~kMovementButtonMask) | movementMask;
    const uint64_t pressed = (pc->buttonstates.m_pButtonStates[1] & ~kMovementButtonMask) | pressedMask;
    const uint64_t released = (pc->buttonstates.m_pButtonStates[2] & ~movementMask) | releasedMask;

    CInButtonStatePB* buttons = base->mutable_buttons_pb();
    buttons->set_buttonstate1(held);
    buttons->set_buttonstate2(pressed);
    buttons->set_buttonstate3(released);

    pc->buttonstates.m_pButtonStates[0] = held;
    pc->buttonstates.m_pButtonStates[1] = pressed;
    pc->buttonstates.m_pButtonStates[2] = released;

    base->set_forwardmove(movement.forwardMove * kUsercmdKeyboardMoveScale);
    base->set_leftmove(movement.leftMove * kUsercmdKeyboardMoveScale);

    g_usercmdMovementApplyCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastUsercmdMovementSlot.store(slot, std::memory_order_relaxed);
    g_lastUsercmdForwardMove.store(static_cast<int>(std::lround(movement.forwardMove * kUsercmdKeyboardMoveScale)),
                                   std::memory_order_relaxed);
    g_lastUsercmdLeftMove.store(static_cast<int>(std::lround(movement.leftMove * kUsercmdKeyboardMoveScale)), std::memory_order_relaxed);

    for (int index = 0; index < base->subtick_moves_size(); ++index)
    {
        CSubtickMoveStep* step = base->mutable_subtick_moves(index);
        step->set_analog_forward_delta(0.0F);
        step->set_analog_left_delta(0.0F);
    }

    return true;
}

// Merges active injections and emits aggregate press and release edges
bool ApplyUsercmdInjections(int slot, PlayerCommand* pc, CBaseUserCmdPB* base)
{
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || !pc || !base) return false;

    uint64_t activeMask = 0;
    uint64_t previousMask = 0;

    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        auto& injections = g_usercmdInjections[slot];
        const int64_t nowMs = MonotonicMilliseconds();

        for (auto it = injections.begin(); it != injections.end();)
        {
            const UsercmdInjectionPhase phase = it->phase;

            if (phase == UsercmdInjectionPhase::PendingRelease || (phase == UsercmdInjectionPhase::Holding && nowMs >= it->expiresAtMs))
            {
                it = injections.erase(it);
                continue;
            }

            activeMask |= it->buttonMask;

            if (phase == UsercmdInjectionPhase::PendingPress)
            {
                if (it->durationMs > 0) it->expiresAtMs = nowMs + it->durationMs;

                it->phase = it->durationMs == 0 ? UsercmdInjectionPhase::PendingRelease : UsercmdInjectionPhase::Holding;
            }

            ++it;
        }

        previousMask = g_injectedHeldMasks[slot];
        g_injectedHeldMasks[slot] = activeMask;
    }

    const uint64_t pressedMask = activeMask & ~previousMask;
    const uint64_t releasedMask = previousMask & ~activeMask;

    uint64_t held = pc->buttonstates.m_pButtonStates[0];
    uint64_t pressed = pc->buttonstates.m_pButtonStates[1];
    uint64_t released = pc->buttonstates.m_pButtonStates[2];

    held = (held & ~releasedMask) | activeMask;
    pressed = (pressed & ~releasedMask) | pressedMask;
    released = (released & ~pressedMask) | releasedMask;

    CInButtonStatePB* buttons = base->mutable_buttons_pb();
    buttons->set_buttonstate1(held);
    buttons->set_buttonstate2(pressed);
    buttons->set_buttonstate3(released);

    pc->buttonstates.m_pButtonStates[0] = held;
    pc->buttonstates.m_pButtonStates[1] = pressed;
    pc->buttonstates.m_pButtonStates[2] = released;

    return true;
}

// Removes suppressed buttons after Bot AI has produced the final command
bool ApplyUsercmdSuppressions(int slot, PlayerCommand* pc, CBaseUserCmdPB* base) // NOLINT(readability-non-const-parameter)
{
    if (!g_installed || !ValidSlotIndex(slot) || !bot_controller_hooks::IsLiveBotSlot(slot) || !pc || !base) return false;

    uint64_t suppressedMask = 0;
    uint64_t releasedMask = 0;

    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        auto& suppressions = g_usercmdSuppressions[slot];
        const int64_t nowMs = MonotonicMilliseconds();

        for (auto it = suppressions.begin(); it != suppressions.end();)
        {
            if (!it->persistent && nowMs >= it->expiresAtMs)
            {
                it = suppressions.erase(it);
                continue;
            }

            suppressedMask |= it->buttonMask;

            if (it->releasePending)
            {
                releasedMask |= it->buttonMask;
                it->releasePending = false;
            }

            ++it;
        }
    }

    if (suppressedMask == 0) return false;

    const uint64_t held = pc->buttonstates.m_pButtonStates[0] & ~suppressedMask;
    const uint64_t pressed = pc->buttonstates.m_pButtonStates[1] & ~suppressedMask;
    const uint64_t released = pc->buttonstates.m_pButtonStates[2] | releasedMask;

    CInButtonStatePB* buttons = base->mutable_buttons_pb();
    buttons->set_buttonstate1(held);
    buttons->set_buttonstate2(pressed);
    buttons->set_buttonstate3(released);

    pc->buttonstates.m_pButtonStates[0] = held;
    pc->buttonstates.m_pButtonStates[1] = pressed;
    pc->buttonstates.m_pButtonStates[2] = released;

    for (int i = 0; i < base->subtick_moves_size(); ++i)
    {
        CSubtickMoveStep* move = base->mutable_subtick_moves(i);
        const uint64_t buttonsMask = move->button() & ~suppressedMask;
        move->set_button(buttonsMask);

        if (buttonsMask == 0) move->set_pressed(false);
    }

    return true;
}

// Applies one recorded G-drop event through the bot client-command path
void ApplyReplayDrop(int slot, void* services)
{
    ReplayDropEvent event{};
    if (!motion_recorder::TakeCurrentReplayDrop(slot, event)) return;
    motion_recorder::DropReplayEventWeapon(slot, services, event);
}

// ---- ProcessMovement: record pre/post + replay pre ----

// Defined after HookedFinishMove
void EnsureVtableHooks(void* services);

// Captures the pre-state and retains invocation-local recording ownership.
KHook::Return<void> HookedProcessMovement(void* services, void* moveData) noexcept
{
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);

    const int slot = ServicesToSlot(services);
    g_lastSlot.store(slot, std::memory_order_relaxed);

    const bool validSlot = ValidSlotIndex(slot);
    const bool recording = validSlot && motion_recorder::IsRecording(slot);

    // Bot-only control path. Recording is intentionally allowed for a human
    // source slot because recordings are later replayed on bots.
    const bool botSlot = validSlot && bot_controller_hooks::IsLiveBotSlot(slot);
    const bool replaying = botSlot && motion_recorder::IsReplaying(slot);

    // FinishMove / PlayerRunCommand are only needed when this subsystem has
    // actual work for this player. Do not initialize the lazy vtable hooks
    // just because an unrelated human moved.
    if (recording || botSlot) EnsureVtableHooks(services);

    // PhysicsSimulate needs slot -> services only for recording or bot control.
    if ((recording || botSlot) && validSlot) g_slotServices[slot].store(services, std::memory_order_release);

    // Recording weapon tap.
    if (recording)
    {
        motion_recorder::SetLiveWs(slot, ServicesToWeaponServices(slot, services));

        if (!g_physicsActive) motion_recorder::OnCapturePre(slot, services, moveData);
    }

    // Replay is strictly bot-only.
    if (replaying) motion_recorder::OnReplayPre(slot, services, moveData);

    // The post hook runs for every ProcessMovement call, so keep a matching
    // lightweight frame even for unrelated humans.
    g_processFrames.push_back({ slot, services, moveData, recording, replaying });

    return { KHook::Action::Ignore };
}

// Completes the matching movement capture after the engine call.
KHook::Return<void> ProcessMovementPost(void*, void*) noexcept
{
    if (g_processFrames.empty()) return { KHook::Action::Ignore };

    const auto [slot, services, moveData, recording, replaying] = g_processFrames.back();
    g_processFrames.pop_back();

    // Recording: commit the tick here only when PhysicsSimulate is not the
    // authoritative per-tick boundary.
    if (recording && !g_physicsActive) motion_recorder::OnCapturePost(slot, services, moveData);

    return { KHook::Action::Ignore };
}

// ---- FinishMove: replay post-write + commit ----

// Applies the recorded end state before FinishMove.
KHook::Return<void> HookedFinishMove(void* services, void* cmd, void* moveData) noexcept
{
    g_finishMoveCalls.fetch_add(1, std::memory_order_relaxed);

    const int slot = ServicesToSlot(services);
    const bool botSlot = ValidSlotIndex(slot) && bot_controller_hooks::IsLiveBotSlot(slot);
    const bool replaying = botSlot && motion_recorder::IsReplaying(slot);

    // Apply commands before FinishMove so their effects belong to this replay
    // tick. Replay is deliberately bot-only.
    if (replaying && !g_physicsActive) ApplyReplayDrop(slot, services);

    // Before original: write post snapshot into MoveData.
    if (replaying) motion_recorder::OnReplayFinishMove(slot, services, moveData);

    g_finishFrames.push_back({ slot, services, moveData, false, replaying });

    return { KHook::Action::Ignore };
}

// Advances the matching replay frame when PhysicsSimulate is unavailable.
KHook::Return<void> FinishMovePost(void*, void*, void*) noexcept
{
    if (g_finishFrames.empty()) return { KHook::Action::Ignore };

    const auto [slot, services, moveData, recording, replaying] = g_finishFrames.back();
    g_finishFrames.pop_back();

    // After original: commit movement and advance the replay cursor only for
    // a bot replay when PhysicsSimulate is unavailable.
    if (replaying && !g_physicsActive)
    {
        motion_recorder::OnReplayCommit(slot, services);
        g_replayCommitCalls.fetch_add(1, std::memory_order_relaxed);
    }

    return { KHook::Action::Ignore };
}

// ---- PlayerRunCommand: subtick record + re-inject ----

// Records or injects the user command before native simulation.
KHook::Return<void> HookedPlayerRunCommand(void* services, void* cmd) noexcept
{
    g_playerRunCommandCalls.fetch_add(1, std::memory_order_relaxed);

    const int slot = ServicesToSlot(services);
    const bool validSlot = ValidSlotIndex(slot);

    // Recording may intentionally target a human source slot.
    const bool recording = validSlot && motion_recorder::IsRecording(slot);

    // Every control operation is bot-only.
    const bool botSlot = validSlot && bot_controller_hooks::IsLiveBotSlot(slot);
    const bool replaying = botSlot && motion_recorder::IsReplaying(slot);

    const bool hasUsercmdInjection = botSlot && HasUsercmdInjection(slot);
    const bool hasUsercmdSuppression = botSlot && HasUsercmdSuppression(slot);
    const bool hasUsercmdMovement = botSlot && HasUsercmdMovement(slot);

    // Projectile alignment belongs to replay/bot control. Never process it
    // from an unrelated human PlayerRunCommand callback.
    if (botSlot) projectile_birth_align::ProcessPending();

    // Unlike ProcessMovement this hook has no post callback, so unrelated
    // humans can leave immediately without touching command protobufs.
    if (!cmd || (!recording && !replaying && !hasUsercmdInjection && !hasUsercmdSuppression && !hasUsercmdMovement))
        return { KHook::Action::Ignore };

    {
        // Compiler computes the multiple-inheritance adjust here.
        auto* pc = reinterpret_cast<PlayerCommand*>(cmd);
        CBaseUserCmdPB* base = pc->mutable_base();

        if (recording)
        {
            // Read this tick's subtick_moves into SubtickMove[] and
            // stash; OnCapturePost (PhysicsSimulate-post) commits them.
            int n = base->subtick_moves_size();
            n = std::min(n, motion_recorder::kMaxSubtickPerTick);
            SubtickMove moves[motion_recorder::kMaxSubtickPerTick];
            for (int i = 0; i < n; ++i)
            {
                const CSubtickMoveStep& s = base->subtick_moves(i);
                moves[i].when = s.when();
                moves[i].button = static_cast<uint32_t>(s.button());
                moves[i].pressed = s.pressed() ? 1.0F : 0.0F;
                moves[i].analogForward = s.analog_forward_delta();
                moves[i].analogLeft = s.analog_left_delta();
                moves[i].pitchDelta = s.pitch_delta();
                moves[i].yawDelta = s.yaw_delta();
            }
            motion_recorder::OnCaptureSubticks(slot, moves, n);

            ReplayCommandFrameData command{};
            command.buttons = pc->buttonstates.m_pButtonStates[0];
            command.buttons1 = pc->buttonstates.m_pButtonStates[1];
            command.buttons2 = pc->buttonstates.m_pButtonStates[2];
            command.fields |= motion_recorder::kCommandFieldButtons;
            if (base->has_forwardmove())
            {
                command.forwardMove = base->forwardmove();
                command.fields |= motion_recorder::kCommandFieldForwardMove;
            }
            if (base->has_leftmove())
            {
                command.leftMove = base->leftmove();
                command.fields |= motion_recorder::kCommandFieldLeftMove;
            }
            if (base->has_upmove())
            {
                command.upMove = base->upmove();
                command.fields |= motion_recorder::kCommandFieldUpMove;
            }
            if (base->has_viewangles())
            {
                const CMsgQAngle& view = base->viewangles();
                command.pitch = view.x();
                command.yaw = view.y();
                command.roll = view.z();
                command.fields |= motion_recorder::kCommandFieldViewAngles;
            }
            if (base->has_mousedx() || base->has_mousedy())
            {
                command.mouseDx = base->mousedx();
                command.mouseDy = base->mousedy();
                command.fields |= motion_recorder::kCommandFieldMouse;
            }
            if (base->has_weaponselect())
            {
                command.weaponSelect = base->weaponselect();
                command.fields |= motion_recorder::kCommandFieldWeaponSelect;
            }
            if (pc->has_left_hand_desired())
            {
                command.leftHandDesired = pc->left_hand_desired() ? 1 : 0;
                command.fields |= motion_recorder::kCommandFieldLeftHand;
            }
            motion_recorder::OnCaptureCommand(slot, command);
        }

        if (replaying)
        {
            constexpr uint64_t kGrenadeAttackMask = (1ULL << 0) | (1ULL << 11);
            motion_recorder::ReplayCommandFrame frame{};
            if (motion_recorder::ReplayCommandFrameForSimulation(slot, frame))
            {
                bool suppressUnsafeUtilityAttack =
                    IsThrowableUtilityDef(frame.tick.weaponDefIndex) && frame.weaponSelect < 0 &&
                    !motion_recorder::ReplayWeaponDefsMatch(motion_recorder::BotActiveWeaponDef(slot), frame.tick.weaponDefIndex);
                if (suppressUnsafeUtilityAttack)
                {
                    frame.buttons0 &= ~kGrenadeAttackMask;
                    frame.buttons1 &= ~kGrenadeAttackMask;
                    frame.buttons2 &= ~kGrenadeAttackMask;
                }

                CInButtonStatePB* bp = base->mutable_buttons_pb();
                bp->set_buttonstate1(frame.buttons0);
                bp->set_buttonstate2(frame.buttons1);
                bp->set_buttonstate3(frame.buttons2);
                pc->buttonstates.m_pButtonStates[0] = frame.buttons0;
                pc->buttonstates.m_pButtonStates[1] = frame.buttons1;
                pc->buttonstates.m_pButtonStates[2] = frame.buttons2;

                CMsgQAngle* view = base->mutable_viewangles();
                view->set_x(frame.commandView.pitch);
                view->set_y(NormalizeDeg(frame.commandView.yaw));
                view->set_z((frame.commandFields & motion_recorder::kCommandFieldViewAngles) != 0 ? frame.commandView.roll : 0.0F);

                if ((frame.commandFields & motion_recorder::kCommandFieldForwardMove) != 0) base->set_forwardmove(frame.forwardMove);
                if ((frame.commandFields & motion_recorder::kCommandFieldLeftMove) != 0) base->set_leftmove(frame.leftMove);
                if ((frame.commandFields & motion_recorder::kCommandFieldUpMove) != 0) base->set_upmove(frame.upMove);
                if ((frame.commandFields & motion_recorder::kCommandFieldMouse) != 0)
                {
                    base->set_mousedx(frame.mouseDx);
                    base->set_mousedy(frame.mouseDy);
                }
                if ((frame.commandFields & motion_recorder::kCommandFieldLeftHand) != 0)
                    pc->set_left_hand_desired(frame.leftHandDesired != 0);
                if (frame.weaponSelect >= 0) base->set_weaponselect(frame.weaponSelect);

                // Replace the command's subtick moves with the recorded set for this tick
                base->clear_subtick_moves();
                for (int i = 0; i < frame.subtickCount; ++i)
                {
                    uint32_t button = frame.subticks[i].button;
                    float pressed = frame.subticks[i].pressed;
                    if (suppressUnsafeUtilityAttack && (button & static_cast<uint32_t>(kGrenadeAttackMask)) != 0)
                    {
                        button &= ~static_cast<uint32_t>(kGrenadeAttackMask);
                        if (button == 0) pressed = 0.0F;
                    }

                    CSubtickMoveStep* m = base->add_subtick_moves();
                    m->set_when(frame.subticks[i].when);
                    m->set_button(button);
                    if (button != 0) // digital press/release
                        m->set_pressed(pressed != 0.0F);
                    if (frame.subticks[i].pitchDelta != 0.0F) m->set_pitch_delta(frame.subticks[i].pitchDelta);
                    if (frame.subticks[i].yawDelta != 0.0F) m->set_yaw_delta(frame.subticks[i].yawDelta);
                    if (frame.subticks[i].analogForward != 0.0F) m->set_analog_forward_delta(frame.subticks[i].analogForward);
                    if (frame.subticks[i].analogLeft != 0.0F) m->set_analog_left_delta(frame.subticks[i].analogLeft);
                }

                motion_recorder::OnReplayCommandPre(slot, services, frame.tick, frame.commandView);
            }
        }

        if (hasUsercmdSuppression && !replaying) ApplyUsercmdSuppressions(slot, pc, base);
        if (hasUsercmdInjection && !replaying) ApplyUsercmdInjections(slot, pc, base);
        if (hasUsercmdMovement && !replaying) ApplyUsercmdMovement(slot, pc, base);
    }

    return { KHook::Action::Ignore };
}

// ---- PhysicsSimulate: the per-tick boundary ----
// Records pre/post + commits

// Captures the per-tick state before any subtick movement.
KHook::Return<void> HookedPhysicsSimulate(void* controller) noexcept
{
    g_physicsSimulateCalls.fetch_add(1, std::memory_order_relaxed);

    const int slot = ControllerToSlot(controller);
    g_lastPhysicsSlot.store(slot, std::memory_order_relaxed);

    const bool validSlot = ValidSlotIndex(slot);

    void* services = validSlot ? g_slotServices[slot].load(std::memory_order_acquire) : nullptr;

    const bool recording = validSlot && services && motion_recorder::IsRecording(slot);

    const bool botSlot = validSlot && bot_controller_hooks::IsLiveBotSlot(slot);

    const bool replaying = botSlot && services && motion_recorder::IsReplaying(slot);

    // Projectile alignment is part of bot replay/control only.
    if (botSlot) projectile_birth_align::ProcessPending();

    // Pre: snapshot start-of-tick state once, before any subtick mover.
    // Recording is intentionally allowed for a human source slot.
    if (recording) motion_recorder::OnCapturePre(slot, services, nullptr);

    // Client commands are normally handled before this tick's player
    // simulation. Replay is bot-only.
    if (replaying) ApplyReplayDrop(slot, services);

    // The post hook runs for every controller simulation call.
    g_physicsFrames.push_back({ slot, services, nullptr, recording, replaying });

    return { KHook::Action::Ignore };
}

// Commits the recording and replay state for the matching simulation call.
KHook::Return<void> PhysicsSimulatePost(void*) noexcept
{
    if (g_physicsFrames.empty()) return { KHook::Action::Ignore };

    const auto [slot, services, moveData, recording, replaying] = g_physicsFrames.back();
    g_physicsFrames.pop_back();

    // Post: snapshot end-of-tick state + commit one frame.
    if (recording) motion_recorder::OnCapturePost(slot, services, nullptr);

    if (replaying)
    {
        motion_recorder::OnReplayCommit(slot, services);
        g_replayCommitCalls.fetch_add(1, std::memory_order_relaxed);
    }

    return { KHook::Action::Ignore };
}

std::atomic<bool> g_vtHooksTried{ false };

void EnsureVtableHooks(void* services)
{
    if (!g_installed || !services) return;

    if (g_vtHooksTried.load(std::memory_order_acquire)) return;

    void** vt = nullptr;

    // Do not permanently mark the lazy-hook attempt as consumed until a
    // readable vtable has actually been obtained. A transient/null services
    // pointer must not disable FinishMove/PlayerRunCommand for the rest of the
    // activation cycle.
    if (!GuardedRead(services, 0, vt) || !vt) return;

    bool expected = false;
    if (!g_vtHooksTried.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    g_addrFinishMove = nullptr;
    g_addrPlayerRunCommand = nullptr;
    g_subtickActive = false;

    // FinishMove
    if (!GuardedRead(static_cast<const void*>(vt), tg::g_vtIdxFinishMove * static_cast<int>(sizeof(void*)), g_addrFinishMove))
    {
        g_addrFinishMove = nullptr;
    }

    if (g_addrFinishMove && !g_hookFinishMove.Install(g_addrFinishMove, &HookedFinishMove, &FinishMovePost))
    {
        g_hookFinishMove.Remove();
        g_addrFinishMove = nullptr;
    }

    // PlayerRunCommand (subtick recording + bot-only replay/injection).
    if (!GuardedRead(static_cast<const void*>(vt), tg::g_vtIdxPlayerRunCommand * static_cast<int>(sizeof(void*)), g_addrPlayerRunCommand))
    {
        g_addrPlayerRunCommand = nullptr;
    }

    if (g_addrPlayerRunCommand && g_hookPlayerRunCommand.Install(g_addrPlayerRunCommand, &HookedPlayerRunCommand))
    {
        g_subtickActive = true;
    }
    else
    {
        g_hookPlayerRunCommand.Remove();
        g_addrPlayerRunCommand = nullptr;
        g_subtickActive = false;
    }
}

} // namespace

bool Install( // NOLINT(misc-use-internal-linkage)
    const nlohmann::json& gd,
    const sig::ModuleInfo& serverModule,
    char* errorOut,
    size_t errorOutLen) // NOLINT(misc-use-internal-linkage)
{
    // Idempotent: never install a second hook set.
    if (g_installed)
    {
        g_status = "ok";
        return true;
    }

    // Defensive rollback of any partial previous attempt.
    Remove();

    g_addrProcessMovement = sig::ResolveSig(gd, serverModule, "CCSPlayer_MovementServices::ProcessMovement", errorOut, errorOutLen);

    if (!g_addrProcessMovement)
    {
        g_status = "failed: ProcessMovement sig";
        return false;
    }

    if (!g_hookProcessMovement.Install(g_addrProcessMovement, &HookedProcessMovement, &ProcessMovementPost))
    {
        Remove();

        if (errorOut && errorOutLen > 0) std::snprintf(errorOut, errorOutLen, "hook ProcessMovement failed");

        g_status = "failed: hook ProcessMovement";
        return false;
    }

    // PhysicsSimulate: optional authoritative per-tick boundary.
    char psErr[256] = { 0 };

    g_addrPhysicsSimulate = sig::ResolveSig(gd, serverModule, "CBasePlayerController::OnSimulateUserCommands", psErr, sizeof(psErr));

    if (g_addrPhysicsSimulate && g_hookPhysicsSimulate.Install(g_addrPhysicsSimulate, &HookedPhysicsSimulate, &PhysicsSimulatePost))
    {
        g_physicsActive = true;
    }
    else
    {
        g_hookPhysicsSimulate.Remove();
        g_addrPhysicsSimulate = nullptr;
        g_physicsActive = false;

        Warning("[BotController] PhysicsSimulate hook unavailable (%s); "
                "replay falls back to per-subtick boundary (may stutter)\n",
                psErr[0] ? psErr : "KHook failed");
    }

    // FinishMove and PlayerRunCommand remain lazy. They are discovered only
    // when ProcessMovement sees either a recording slot or a confirmed bot.
    g_installed = true;
    g_status = "ok";

    return true;
}

void Remove()
{
    // Disable API-visible state first so no new injection/replay control work
    // can be accepted while hooks are being drained.
    g_installed = false;
    g_subtickActive = false;

    // Remove in reverse/lazy dependency order. NativeHook::Remove() is safe
    // when the hook is absent and waits for active invocations to drain.
    g_hookPlayerRunCommand.Remove();
    g_hookFinishMove.Remove();
    g_hookPhysicsSimulate.Remove();
    g_hookProcessMovement.Remove();

    // Keep the old boundary semantics until all in-flight callbacks have
    // drained; only then publish that PhysicsSimulate is unavailable.
    g_physicsActive = false;

    g_addrProcessMovement = nullptr;
    g_addrFinishMove = nullptr;
    g_addrPlayerRunCommand = nullptr;
    g_addrPhysicsSimulate = nullptr;

    // Next Enable() must rediscover the live MovementServices vtable.
    g_vtHooksTried.store(false, std::memory_order_release);

    // Live engine pointers must never survive runtime-off.
    for (auto& services : g_slotServices)
        services.store(nullptr, std::memory_order_release);

    for (auto& pawn : g_slotPawns)
        pawn.store(nullptr, std::memory_order_release);

    // Clear all deferred usercmd state.
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);

        for (auto& injections : g_usercmdInjections)
            injections.clear();

        for (auto& suppressions : g_usercmdSuppressions)
            suppressions.clear();

        for (auto& movements : g_usercmdMovements)
            movements.clear();

        g_injectedHeldMasks.fill(0);
        g_movementHeldMasks.fill(0);
    }

    // KHook callbacks are expected on the server/game thread. Once Remove()
    // has drained them, no invocation-local frame should survive this runtime
    // cycle.
    g_processFrames.clear();
    g_finishFrames.clear();
    g_physicsFrames.clear();

    g_status = "not_attempted";
}

const char* Status() { return g_status.c_str(); }

void* ProcessUsercmdAddress() { return g_addrProcessMovement; }

uint64_t HookCallCount() { return g_hookCalls.load(std::memory_order_relaxed); }
int LastResolvedSlot() { return g_lastSlot.load(std::memory_order_relaxed); }
uint64_t FinishMoveCallCount() { return g_finishMoveCalls.load(std::memory_order_relaxed); }
uint64_t PlayerRunCommandCallCount() { return g_playerRunCommandCalls.load(std::memory_order_relaxed); }
// Reports how many final bot commands received a movement override
uint64_t UsercmdMovementApplyCount() { return g_usercmdMovementApplyCalls.load(std::memory_order_relaxed); }
// Reports the last slot whose final bot command was overridden
int LastUsercmdMovementSlot() { return g_lastUsercmdMovementSlot.load(std::memory_order_relaxed); }
// Reports the last forward command magnitude written by the override
int LastUsercmdForwardMove() { return g_lastUsercmdForwardMove.load(std::memory_order_relaxed); }
// Reports the last left command magnitude written by the override
int LastUsercmdLeftMove() { return g_lastUsercmdLeftMove.load(std::memory_order_relaxed); }
uint64_t PhysicsSimulateCallCount() { return g_physicsSimulateCalls.load(std::memory_order_relaxed); }
int LastPhysicsSlot() { return g_lastPhysicsSlot.load(std::memory_order_relaxed); }
uint64_t ReplayCommitCount() { return g_replayCommitCalls.load(std::memory_order_relaxed); }
uint64_t SlotResolveCallCount() { return g_slotResolveCalls.load(std::memory_order_relaxed); }
uint64_t SlotResolveFailureCount() { return g_slotResolveFailures.load(std::memory_order_relaxed); }
uintptr_t LastServices() { return g_lastServices.load(std::memory_order_relaxed); }
uintptr_t LastPawn() { return g_lastPawn.load(std::memory_order_relaxed); }
uint32_t LastControllerHandle() { return g_lastControllerHandle.load(std::memory_order_relaxed); }
uint32_t LastOriginalControllerHandle() { return g_lastOriginalControllerHandle.load(std::memory_order_relaxed); }
int LastControllerIndex() { return g_lastControllerIndex.load(std::memory_order_relaxed); }
int LastOriginalControllerIndex() { return g_lastOriginalControllerIndex.load(std::memory_order_relaxed); }
int LastOwnerSlot() { return g_lastOwnerSlot.load(std::memory_order_relaxed); }
} // namespace input_injector
} // namespace cs2bc
