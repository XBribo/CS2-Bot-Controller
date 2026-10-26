#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/log.h"
// CS2 movement hooks
// OnSimulateUserCommands (controller batch context)
// ProcessMovement (live services discovery)
// PlayerRunCommand(subtick record + re-inject)

#include "networkbasetypes.pb.h"
#include "nlohmann/json.hpp" // NOLINT(misc-include-cleaner)
#include "playercommand.h"

#include "InputInjector.h"
#include "CommandHistory.h"
#include "PawnBinding.h"
#include "ccsbot_slot.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include "MotionRecorder.h"
#include "usercmd.pb.h"
#include "offsets.h"
#include "core/cs2_sdk/src/utils/hooks.h"
#include "WeaponLocker.h"

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

namespace tg = cs2bc::offsets;

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
void* g_addrPlayerRunCommand = nullptr;
void* g_addrSimulateUserCommands = nullptr;

cs2_sdk::hooks::NativeHook<void, void*, void*> g_hookProcessMovement;
cs2_sdk::hooks::NativeHook<void, void*, void*> g_hookPlayerRunCommand;
cs2_sdk::hooks::NativeHook<void, void*> g_hookSimulateUserCommands;
cs2_sdk::hooks::NativeHook<void, void*> g_hookControllerCommandSetup;
std::atomic<bool> g_controllerHookTried{ false };

struct CommandBatch
{
    int slot;
    void* services;
    uint64_t generation = 0;
    bool capturePrepared = false;
    bool preparing = false;
};
thread_local std::vector<CommandBatch> g_commandBatches;
struct CommandFrame
{
    int slot = -1;
    void* services = nullptr;
    uint64_t generation = 0;
    bool recording = false;
    bool replayApplied = false;
};
thread_local std::vector<CommandFrame> g_commandFrames;
bool g_installed = false;
// True once the controller batch entry is hooked.
bool g_batchActive = false;
// True once PlayerRunCommand is hooked
bool g_usercmdActive = false;
std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

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
std::atomic<uint64_t> g_usercmdWorkSlots{ 0 };
std::atomic<int64_t> g_nextUsercmdInjectionId{ 1 };
std::atomic<int64_t> g_nextUsercmdSuppressionId{ 1 };
std::atomic<int64_t> g_nextUsercmdMovementId{ 1 };

bool IsThrowableUtilityDef(int def) { return def >= 43 && def <= 48; }

// Reports whether a slot can index the fixed replay state arrays.
bool ValidSlotIndex(int slot) { return slot >= 0 && slot < kMaxSlots; }

// Publishes whether a slot still has command overrides after a locked mutation.
void RefreshUsercmdWorkSlot(int slot)
{
    const uint64_t bit = uint64_t{ 1 } << slot;
    if (!g_usercmdInjections[slot].empty() || !g_usercmdSuppressions[slot].empty() || !g_usercmdMovements[slot].empty())
        g_usercmdWorkSlots.fetch_or(bit, std::memory_order_release);
    else
        g_usercmdWorkSlots.fetch_and(~bit, std::memory_order_release);
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
    if (!ValidSlotIndex(slot) || buttonMask == 0 || durationMs < 0 || !g_usercmdActive || motion_recorder::IsReplaying(slot)) return -1;
    if (!pawn_binding::BindCurrentPawn(slot)) return -1;

    int64_t id = g_nextUsercmdInjectionId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdInjections[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .durationMs = durationMs, .phase = UsercmdInjectionPhase::PendingPress });
    RefreshUsercmdWorkSlot(slot);
    return id;
}

// Creates an independently cancellable persistent analog movement override
int64_t StartUsercmdMovement(int slot, float forwardMove, float leftMove)
{
    if (!ValidSlotIndex(slot) || !std::isfinite(forwardMove) || !std::isfinite(leftMove) || !g_usercmdActive ||
        motion_recorder::IsReplaying(slot))
        return -1;

    if (!pawn_binding::BindCurrentPawn(slot)) return -1;
    int64_t id = g_nextUsercmdMovementId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdMovements[slot].push_back(
        { .id = id, .forwardMove = std::clamp(forwardMove, -1.0F, 1.0F), .leftMove = std::clamp(leftMove, -1.0F, 1.0F) });
    RefreshUsercmdWorkSlot(slot);
    return id;
}

// Updates one persistent analog movement override
bool UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove)
{
    if (!ValidSlotIndex(slot) || movementId <= 0 || !std::isfinite(forwardMove) || !std::isfinite(leftMove)) return false;
    if (!pawn_binding::ServicesForSlot(slot)) return false;

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
        RefreshUsercmdWorkSlot(slot);
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
        RefreshUsercmdWorkSlot(slot);
        return true;
    }
    return false;
}

// Suppresses selected usercmd buttons until the requested duration expires
bool SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || durationMs <= 0 || !g_usercmdActive || motion_recorder::IsReplaying(slot)) return false;
    if (!pawn_binding::BindCurrentPawn(slot)) return false;

    int64_t expiresAtMs = MonotonicMilliseconds() + durationMs;
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return false;
    g_usercmdSuppressions[slot].push_back(
        { .id = 0, .buttonMask = buttonMask, .expiresAtMs = expiresAtMs, .persistent = false, .releasePending = true });
    RefreshUsercmdWorkSlot(slot);
    return true;
}

// Creates an independently cancellable persistent usercmd suppression
int64_t StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || !g_usercmdActive || motion_recorder::IsReplaying(slot)) return -1;
    if (!pawn_binding::BindCurrentPawn(slot)) return -1;

    int64_t id = g_nextUsercmdSuppressionId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdSuppressions[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .persistent = true, .releasePending = true });
    RefreshUsercmdWorkSlot(slot);
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
        RefreshUsercmdWorkSlot(slot);
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
    RefreshUsercmdWorkSlot(slot);
    g_injectedHeldMasks[slot] = 0;
    g_movementHeldMasks[slot] = 0;
}

// Queries pending input ownership without changing press/release state.
namespace {

struct UsercmdWork
{
    bool injection;
    bool suppression;
    bool movement;
};

// Takes one consistent snapshot instead of locking once per input kind.
UsercmdWork GetUsercmdWork(int slot)
{
    if (!ValidSlotIndex(slot)) return {};

    std::scoped_lock lock(g_usercmdInjectionMutex);
    return { !g_usercmdInjections[slot].empty(), !g_usercmdSuppressions[slot].empty(), !g_usercmdMovements[slot].empty() };
}

// Replaces Bot AI analog movement after the final command is generated
bool ApplyUsercmdMovement(int slot, PlayerCommand* pc, CBaseUserCmdPB* base) // NOLINT(readability-non-const-parameter)
{
    if (!ValidSlotIndex(slot) || pc == nullptr || base == nullptr) return false;

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

    uint64_t pressedMask = movementMask & ~previousMask;
    uint64_t releasedMask = previousMask & ~movementMask;
    uint64_t held = (pc->buttonstates.m_pButtonStates[0] & ~kMovementButtonMask) | movementMask;
    uint64_t pressed = (pc->buttonstates.m_pButtonStates[1] & ~kMovementButtonMask) | pressedMask;
    uint64_t released = (pc->buttonstates.m_pButtonStates[2] & ~movementMask) | releasedMask;

    CInButtonStatePB* buttons = base->mutable_buttons_pb();
    buttons->set_buttonstate1(held);
    buttons->set_buttonstate2(pressed);
    buttons->set_buttonstate3(released);
    pc->buttonstates.m_pButtonStates[0] = held;
    pc->buttonstates.m_pButtonStates[1] = pressed;
    pc->buttonstates.m_pButtonStates[2] = released;

    base->set_forwardmove(movement.forwardMove * kUsercmdKeyboardMoveScale);
    base->set_leftmove(movement.leftMove * kUsercmdKeyboardMoveScale);
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
    if (!ValidSlotIndex(slot) || !pc || !base) return false;

    uint64_t activeMask = 0;
    uint64_t previousMask = 0;
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        auto& injections = g_usercmdInjections[slot];
        int64_t nowMs = MonotonicMilliseconds();
        for (auto it = injections.begin(); it != injections.end();)
        {
            UsercmdInjectionPhase phase = it->phase;
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
        RefreshUsercmdWorkSlot(slot);
    }

    uint64_t pressedMask = activeMask & ~previousMask;
    uint64_t releasedMask = previousMask & ~activeMask;
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
    if (!ValidSlotIndex(slot) || !pc || !base) return false;

    uint64_t suppressedMask = 0;
    uint64_t releasedMask = 0;
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        auto& suppressions = g_usercmdSuppressions[slot];
        int64_t nowMs = MonotonicMilliseconds();
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
        RefreshUsercmdWorkSlot(slot);
    }

    if (suppressedMask == 0) return false;

    uint64_t held = pc->buttonstates.m_pButtonStates[0] & ~suppressedMask;
    uint64_t pressed = pc->buttonstates.m_pButtonStates[1] & ~suppressedMask;
    uint64_t released = pc->buttonstates.m_pButtonStates[2] | releasedMask;

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
        uint64_t buttonsMask = move->button() & ~suppressedMask;
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

// Installs the command hook from the live movement-services vtable.
void EnsureVtableHooks(void* services);

// Discovers command hooks and records the registered pawn's weapon service.
KHook::Return<void> HookedProcessMovement(void* services, void*) noexcept
{
    EnsureVtableHooks(services);
    if (!motion_recorder::HasAnyRecording()) return { KHook::Action::Ignore };
    int slot = pawn_binding::ServicesToSlot(services);
    if (motion_recorder::IsRecording(slot))
        motion_recorder::SetLiveWs(slot, pawn_binding::ServicesToWeaponServices(slot, services));
    return { KHook::Action::Ignore };
}

// Includes inactive nested batches so they cannot borrow an outer context.
CommandBatch* FindCommandBatch(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    for (auto it = g_commandBatches.rbegin(); it != g_commandBatches.rend(); ++it)
        if (it->slot == slot) return &*it;
    return nullptr;
}

// Keeps recursive commands from consuming the active command's buffers.
CommandFrame* FindCommandFrame(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    for (auto it = g_commandFrames.rbegin(); it != g_commandFrames.rend(); ++it)
        if (it->slot == slot) return &*it;
    return nullptr;
}

// ---- PlayerRunCommand: subtick record + re-inject ----

// Serializes command fields and subticks into the pending recording frame.
void CaptureUserCommand(int slot, void* services, PlayerCommand* pc, CBaseUserCmdPB* base)
{
    std::string history;
    int simulationTick;
    if (!pawn_binding::CommandSimulationTick(slot, services, *pc, simulationTick) ||
        !command_history::Capture(*pc, simulationTick, history))
    {
        BC_LOG_WARN("slot %d: invalid command history; recording stopped", slot);
        motion_recorder::StopRecord(slot);
        return;
    }
    // Stash subticks until this PlayerRunCommand returns.
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
    command.fields |= motion_recorder::kCommandFieldWeaponSelectDef;
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
        command.weaponSelect = base->weaponselect() > 0
                                   ? weapon_locker_hooks::WeaponDefForEntityIndex(
                                         pawn_binding::ServicesToWeaponServices(slot, services), base->weaponselect())
                                   : 0;
        command.fields |= motion_recorder::kCommandFieldWeaponSelect;
    }
    if (pc->has_left_hand_desired())
    {
        command.leftHandDesired = pc->left_hand_desired() ? 1 : 0;
        command.fields |= motion_recorder::kCommandFieldLeftHand;
    }
    motion_recorder::OnCaptureCommand(slot, command, std::move(history));
}

// Applies one recorded command before the engine simulates it.
bool ApplyReplayUserCommand(int slot, void* services, PlayerCommand* pc, CBaseUserCmdPB* base)
{
    constexpr uint64_t kGrenadeAttackMask = (1ULL << 0) | (1ULL << 11);
    motion_recorder::ReplayCommandFrame frame{};
    if (motion_recorder::ReplayCommandFrameForSimulation(slot, services, frame))
    {
        const auto* active = FindCommandFrame(slot);
        if (!active || active->services != services || active->generation != motion_recorder::PawnGeneration(slot) ||
            !motion_recorder::IsReplaying(slot))
            return false;
        const uint64_t generation = active->generation;
        bool suppressUnsafeUtilityAttack =
            IsThrowableUtilityDef(frame.tick.weaponDefIndex) && frame.weaponSelect < 0 &&
            !motion_recorder::ReplayWeaponDefsMatch(
                weapon_locker_hooks::ActiveWeaponDef(pawn_binding::ServicesToWeaponServices(slot, services)), frame.tick.weaponDefIndex);
        if (suppressUnsafeUtilityAttack)
        {
            frame.buttons0 &= ~kGrenadeAttackMask;
            frame.buttons1 &= ~kGrenadeAttackMask;
            frame.buttons2 &= ~kGrenadeAttackMask;
        }
        if (!motion_recorder::IsReplaying(slot) || generation != motion_recorder::PawnGeneration(slot)) return false;
        if (!motion_recorder::OnReplayCommandPre(slot, services, frame.tick)) return false;

        if (frame.inputHistory.has_base())
        {
            int simulationTick;
            if (!pawn_binding::CommandSimulationTick(slot, services, *pc, simulationTick) ||
                !command_history::Apply(frame.inputHistory, *pc, simulationTick))
            {
                BC_LOG_WARN("slot %d: invalid command clock; replay stopped", slot);
                motion_recorder::StopReplay(slot);
                return false;
            }
        }
        else
            command_history::Clear(*pc);
        if (suppressUnsafeUtilityAttack)
        {
            pc->set_attack1_start_history_index(-1);
            pc->set_attack2_start_history_index(-1);
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
        if ((frame.commandFields & motion_recorder::kCommandFieldLeftHand) != 0) pc->set_left_hand_desired(frame.leftHandDesired != 0);
        if ((frame.commandFields & motion_recorder::kCommandFieldForwardMove) == 0) base->clear_forwardmove();
        if ((frame.commandFields & motion_recorder::kCommandFieldLeftMove) == 0) base->clear_leftmove();
        if ((frame.commandFields & motion_recorder::kCommandFieldUpMove) == 0) base->clear_upmove();
        if ((frame.commandFields & motion_recorder::kCommandFieldMouse) == 0)
        {
            base->clear_mousedx();
            base->clear_mousedy();
        }
        if ((frame.commandFields & motion_recorder::kCommandFieldLeftHand) == 0) pc->clear_left_hand_desired();
        if (frame.weaponSelect >= 0) base->set_weaponselect(frame.weaponSelect);
        else
            base->clear_weaponselect();

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

        return motion_recorder::IsReplaying(slot) && generation == motion_recorder::PawnGeneration(slot);
    }
    return false;
}

// Records or injects the user command before native simulation.
KHook::Return<void> PlayerRunCommandPre(void* services, void* cmd) noexcept
{
    const size_t index = g_commandFrames.size();
    g_commandFrames.push_back({});
    if (!motion_recorder::HasAnyRecording() && !motion_recorder::HasAnyReplay() && g_usercmdWorkSlots.load(std::memory_order_acquire) == 0)
        return { KHook::Action::Ignore };
    int slot = pawn_binding::ServicesToSlot(services);
    if (!cmd || FindCommandFrame(slot) || !ResolveReplayPawn(slot, services)) return { KHook::Action::Ignore };
    const uint64_t generation = motion_recorder::PawnGeneration(slot);
    auto* batch = FindCommandBatch(slot);
    if (batch && (batch->services != services || batch->generation != generation || batch->preparing))
        return { KHook::Action::Ignore };
    const bool recording = batch && motion_recorder::IsRecording(slot);
    const bool replaying = batch && motion_recorder::IsReplaying(slot);
    const bool capturePrepared = batch && batch->capturePrepared;
    if (batch) batch->capturePrepared = false;
    const auto [hasUsercmdInjection, hasUsercmdSuppression, hasUsercmdMovement] = replaying ? UsercmdWork{} : GetUsercmdWork(slot);

    if (recording || replaying || hasUsercmdInjection || hasUsercmdSuppression || hasUsercmdMovement)
    {
        g_commandFrames[index] = { slot, services, generation, recording, false };
        // Compiler computes the multiple-inheritance adjust here.
        auto* pc = reinterpret_cast<PlayerCommand*>(cmd);
        CBaseUserCmdPB* base = pc->mutable_base();

        if (recording)
        {
            if (!capturePrepared) motion_recorder::OnRecordCommandPre(slot, services);
            CaptureUserCommand(slot, services, pc, base);
        }

        if (replaying)
        {
            const bool applied = ApplyReplayUserCommand(slot, services, pc, base);
            g_commandFrames[index].replayApplied = applied;
        }

        if (hasUsercmdSuppression && !replaying) ApplyUsercmdSuppressions(slot, pc, base);
        if (hasUsercmdInjection && !replaying) ApplyUsercmdInjections(slot, pc, base);
        if (hasUsercmdMovement && !replaying) ApplyUsercmdMovement(slot, pc, base);
    }

    return { KHook::Action::Ignore };
}

// Commits exactly one command, keeping its reentrancy guard through native callbacks.
KHook::Return<void> PlayerRunCommandPost(void*, void*) noexcept
{
    const CommandFrame frame = g_commandFrames.back();
    if ((frame.recording || frame.replayApplied) && ResolveReplayPawn(frame.slot, frame.services) &&
        frame.generation == motion_recorder::PawnGeneration(frame.slot))
    {
        if (frame.recording) motion_recorder::OnRecordCommandPost(frame.slot, frame.services);
        if (frame.replayApplied) motion_recorder::OnReplayCommandPost(frame.slot, frame.services);
    }
    g_commandFrames.pop_back();
    return { KHook::Action::Ignore };
}

// ---- OnSimulateUserCommands: per-controller batch context ----

// Dispatches drops after native command setup, at the client-command boundary.
KHook::Return<void> ControllerCommandSetupPost(void* controller) noexcept
{
    if (g_commandBatches.empty()) return { KHook::Action::Ignore };
    const size_t index = g_commandBatches.size() - 1;
    const CommandBatch batch = g_commandBatches[index];
    if (!batch.services || batch.preparing || FindCommandFrame(batch.slot) || ControllerToSlot(controller) != batch.slot ||
        !ResolveReplayPawn(batch.slot, batch.services) || batch.generation != motion_recorder::PawnGeneration(batch.slot))
        return { KHook::Action::Ignore };
    g_commandBatches[index].preparing = true;
    const bool prepared = motion_recorder::IsRecording(batch.slot) &&
                          motion_recorder::OnRecordCommandPre(batch.slot, batch.services);
    if (motion_recorder::IsReplaying(batch.slot)) ApplyReplayDrop(batch.slot, batch.services);
    g_commandBatches[index].capturePrepared = prepared;
    g_commandBatches[index].preparing = false;
    return { KHook::Action::Ignore };
}

// Resolves the controller's per-command setup independently of movement services.
void EnsureControllerCommandHook(void* controller)
{
    if (!controller || g_controllerHookTried.exchange(true, std::memory_order_acq_rel)) return;
    void** vtable = nullptr;
    void* target = nullptr;
    if (tg::g_vtIdxControllerCommandSetup >= 0 && GuardedRead(controller, 0, vtable) && vtable &&
        GuardedRead(static_cast<const void*>(vtable), tg::g_vtIdxControllerCommandSetup * static_cast<int>(sizeof(void*)), target) &&
        target && g_hookControllerCommandSetup.Install(target, nullptr, &ControllerCommandSetupPost))
        return;
    g_hookControllerCommandSetup.Remove();
    BC_LOG_WARN("record/replay: command setup unavailable");
}

// Publishes batch ownership; individual commands own snapshots and cursor advancement.
KHook::Return<void> SimulateUserCommandsPre(void* controller) noexcept
{
    EnsureControllerCommandHook(controller);
    if (!motion_recorder::HasAnyRecording() && !motion_recorder::HasAnyReplay())
    {
        // Keep the matching post callback from consuming an outer frame.
        g_commandBatches.push_back({ -1, nullptr });
        return { KHook::Action::Ignore };
    }
    int slot = ControllerToSlot(controller);

    void* services = pawn_binding::ServicesForSlot(slot);

    // Nested batches for this slot must not consume the outer command.
    if (FindCommandBatch(slot))
    {
        g_commandBatches.push_back({ slot, nullptr });
        return { KHook::Action::Ignore };
    }
    // Publish ownership before engine callbacks can reenter.
    g_commandBatches.push_back({ slot, services, motion_recorder::PawnGeneration(slot) });

    return { KHook::Action::Ignore };
}

// Empty batches never record or consume a command frame.
KHook::Return<void> SimulateUserCommandsPost(void*) noexcept
{
    g_commandBatches.pop_back();
    return { KHook::Action::Ignore };
}

std::atomic<bool> g_vtHooksTried{ false };

void EnsureVtableHooks(void* services)
{
    if (g_vtHooksTried.load(std::memory_order_acquire)) return;
    if (g_vtHooksTried.exchange(true, std::memory_order_acq_rel)) return;
    if (!services) return;
    void** vt = nullptr;
    if (!GuardedRead(services, 0, vt) || !vt) return;

    // PlayerRunCommand (subtick record/re-inject)
    if (!GuardedRead(static_cast<const void*>(vt), tg::g_vtIdxPlayerRunCommand * static_cast<int>(sizeof(void*)), g_addrPlayerRunCommand))
        g_addrPlayerRunCommand = nullptr;
    if (g_addrPlayerRunCommand && g_hookPlayerRunCommand.Install(g_addrPlayerRunCommand, &PlayerRunCommandPre, &PlayerRunCommandPost))
    {
        g_usercmdActive = true;
    }
    else if (g_addrPlayerRunCommand)
    {
        g_hookPlayerRunCommand.Remove();
        g_addrPlayerRunCommand = nullptr;
    }
}

} // namespace

bool Install( // NOLINT(misc-use-internal-linkage)
    const nlohmann::json& gd,
    const cs2_sdk::modules::ModuleInfo& serverModule,
    char* errorOut,
    size_t errorOutLen) // NOLINT(misc-use-internal-linkage)
{
    g_addrProcessMovement =
        cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "CCSPlayer_MovementServices::ProcessMovement", errorOut, errorOutLen);
    if (!g_addrProcessMovement)
    {
        g_status = "failed: ProcessMovement sig";
        return false;
    }
    if (!g_hookProcessMovement.Install(g_addrProcessMovement, &HookedProcessMovement))
    {
        std::snprintf(errorOut, errorOutLen, "hook ProcessMovement failed");
        g_hookProcessMovement.Remove();
        g_status = "failed: hook ProcessMovement";
        return false;
    }

    // Controller command batches can contain several PlayerRunCommand calls.
    char batchError[256] = { 0 };
    g_addrSimulateUserCommands =
        cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "CBasePlayerController::OnSimulateUserCommands", batchError, sizeof(batchError));
    if (g_addrSimulateUserCommands && g_hookSimulateUserCommands.Install(g_addrSimulateUserCommands, &SimulateUserCommandsPre, &SimulateUserCommandsPost))
    {
        g_batchActive = true;
    }
    else
    {
        if (g_addrSimulateUserCommands)
        {
            g_hookSimulateUserCommands.Remove();
            g_addrSimulateUserCommands = nullptr;
        }
        BC_LOG_WARN("record/replay: batch hook failed (%s)", batchError[0] ? batchError : "KHook");
    }

    // PlayerRunCommand is hooked lazily from the first live movement-services vtable.
    char dropReleaseError[256]{};
    void* outerDrop =
        cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "DropReleasePose::OuterDrop", dropReleaseError, sizeof(dropReleaseError));
    void* buildTransform = outerDrop ? cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "DropReleasePose::BuildTransform",
                                                                       dropReleaseError, sizeof(dropReleaseError))
                                     : nullptr;
    if (!motion_recorder::InstallDropReleasePose(outerDrop, buildTransform))
        BC_LOG_WARN("drop replay unavailable (%s)", dropReleaseError[0] ? dropReleaseError : "hook failed");
    g_installed = true;
    g_status = "ok";
    return true;
}

void Remove()
{
    if (!g_installed) return;
    g_hookProcessMovement.Remove();
    g_hookPlayerRunCommand.Remove();
    g_hookControllerCommandSetup.Remove();
    g_hookSimulateUserCommands.Remove();
    g_addrProcessMovement = nullptr;
    g_addrPlayerRunCommand = nullptr;
    g_addrSimulateUserCommands = nullptr;
    g_batchActive = false;
    g_usercmdActive = false;
    g_vtHooksTried.store(false, std::memory_order_release);
    g_controllerHookTried.store(false, std::memory_order_release);
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
        g_usercmdWorkSlots.store(0, std::memory_order_release);
    }
    g_installed = false;
    g_status = "not_attempted";
}

// Recording and replay require batch, command, and queued-client-command boundaries.
bool RecorderReady()
{
    if (g_batchActive && g_usercmdActive && g_hookControllerCommandSetup.Active()) return true;
    BC_LOG_WARN("record/replay unavailable: batch=%d command=%d setup=%d", g_batchActive,
                g_usercmdActive, g_hookControllerCommandSetup.Active());
    return false;
}

const char* Status() { return g_status.c_str(); }

} // namespace input_injector
} // namespace cs2bc
