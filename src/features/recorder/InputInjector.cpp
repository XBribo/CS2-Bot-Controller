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
};

struct UsercmdControlState
{
    uint64_t mask = 0;
    uint64_t held = 0;
};

std::array<std::vector<UsercmdInjection>, kMaxSlots> g_usercmdInjections{};
std::array<std::vector<UsercmdSuppression>, kMaxSlots> g_usercmdSuppressions{};
std::array<std::vector<UsercmdMovement>, kMaxSlots> g_usercmdMovements{};
std::array<uint64_t, kMaxSlots> g_controlledButtons{};
std::mutex g_usercmdInjectionMutex;
std::atomic<uint64_t> g_usercmdWorkSlots{ 0 };
std::atomic<int64_t> g_nextUsercmdInjectionId{ 1 };
std::atomic<int64_t> g_nextUsercmdSuppressionId{ 1 };
std::atomic<int64_t> g_nextUsercmdMovementId{ 1 };

bool IsThrowableUtilityDef(int def) { return def >= 43 && def <= 48; }

// Checks slot bounds.
bool ValidSlotIndex(int slot) { return slot >= 0 && slot < kMaxSlots; }

// Updates the active-input mask under lock.
void RefreshUsercmdWorkSlot(int slot)
{
    const uint64_t bit = uint64_t{ 1 } << slot;
    if (!g_usercmdInjections[slot].empty() || !g_usercmdSuppressions[slot].empty() || !g_usercmdMovements[slot].empty() ||
        g_controlledButtons[slot] != 0)
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

// Monotonic milliseconds for input expiry.
int64_t MonotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Starts button injection; returns a token.
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

// Starts persistent movement; returns a token.
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

// Updates a movement token.
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

// Cancels a movement token.
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

// Cancels a button-injection token.
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

// Suppresses buttons for a fixed duration.
bool SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || durationMs <= 0 || !g_usercmdActive || motion_recorder::IsReplaying(slot)) return false;
    if (!pawn_binding::BindCurrentPawn(slot)) return false;

    int64_t expiresAtMs = MonotonicMilliseconds() + durationMs;
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return false;
    g_usercmdSuppressions[slot].push_back(
        { .id = 0, .buttonMask = buttonMask, .expiresAtMs = expiresAtMs, .persistent = false });
    RefreshUsercmdWorkSlot(slot);
    return true;
}

// Starts persistent suppression; returns a token.
int64_t StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || !g_usercmdActive || motion_recorder::IsReplaying(slot)) return -1;
    if (!pawn_binding::BindCurrentPawn(slot)) return -1;

    int64_t id = g_nextUsercmdSuppressionId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdSuppressions[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .persistent = true });
    RefreshUsercmdWorkSlot(slot);
    return id;
}

// Cancels a suppression token.
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

// Clears all pending input controls.
void ClearUsercmdInjections(int slot)
{
    if (!ValidSlotIndex(slot)) return;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    g_usercmdInjections[slot].clear();
    g_usercmdSuppressions[slot].clear();
    g_usercmdMovements[slot].clear();
    g_controlledButtons[slot] = 0;
    RefreshUsercmdWorkSlot(slot);
}

// Checks pending input controls.
namespace {

// Composes command input under one state lock.
void ApplyUsercmdControls(int slot, PlayerCommand& pc, CBaseUserCmdPB& base, uint64_t previousHeld)
{
    uint64_t states[3] = { pc.buttonstates.m_pButtonStates[0], pc.buttonstates.m_pButtonStates[1], pc.buttonstates.m_pButtonStates[2] };
    UsercmdControlState forced{};
    UsercmdMovement movement{};
    uint64_t suppressed = 0;
    bool moving = false;
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        const int64_t nowMs = MonotonicMilliseconds();
        auto& injections = g_usercmdInjections[slot];
        for (auto it = injections.begin(); it != injections.end();)
        {
            if (it->phase == UsercmdInjectionPhase::PendingRelease ||
                (it->phase == UsercmdInjectionPhase::Holding && nowMs >= it->expiresAtMs))
            {
                it = injections.erase(it);
                continue;
            }
            forced.mask |= it->buttonMask;
            forced.held |= it->buttonMask;
            if (it->phase == UsercmdInjectionPhase::PendingPress)
            {
                it->expiresAtMs = nowMs + it->durationMs;
                it->phase = it->durationMs == 0 ? UsercmdInjectionPhase::PendingRelease : UsercmdInjectionPhase::Holding;
            }
            ++it;
        }

        const auto& movements = g_usercmdMovements[slot];
        moving = !movements.empty();
        if (moving)
        {
            movement = movements.back();
            uint64_t held = 0;
            if (movement.forwardMove > 0.0F) held |= kInForward;
            else if (movement.forwardMove < 0.0F)
                held |= kInBack;
            if (movement.leftMove > 0.0F) held |= kInMoveLeft;
            else if (movement.leftMove < 0.0F)
                held |= kInMoveRight;
            forced.mask |= kMovementButtonMask;
            forced.held = (forced.held & ~kMovementButtonMask) | held;
        }

        auto& suppressions = g_usercmdSuppressions[slot];
        std::erase_if(suppressions, [nowMs](const auto& suppression) {
            return !suppression.persistent && nowMs >= suppression.expiresAtMs;
        });
        for (const auto& suppression : suppressions)
            suppressed |= suppression.buttonMask;
        forced.mask |= suppressed;
        forced.held &= ~suppressed;

        auto& previousMask = g_controlledButtons[slot];
        const uint64_t rawInitial = states[0] ^ states[1];
        const uint64_t rejoin = (rawInitial ^ previousHeld) & previousMask & ~forced.mask;
        // Button planes encode final hold and a two-bit transition count.
        states[2] |= states[1] & rejoin;
        states[1] ^= rejoin;
        states[0] = (states[0] & ~forced.mask) | forced.held;
        states[1] = (states[1] & ~forced.mask) | ((previousHeld ^ forced.held) & forced.mask);
        states[2] &= ~forced.mask;
        previousMask = forced.mask;
        RefreshUsercmdWorkSlot(slot);
    }

    CInButtonStatePB* buttons = base.mutable_buttons_pb();
    buttons->set_buttonstate1(states[0]);
    buttons->set_buttonstate2(states[1]);
    buttons->set_buttonstate3(states[2]);
    std::copy_n(states, 3, pc.buttonstates.m_pButtonStates);
    if ((suppressed & (1ULL << 0)) != 0) pc.set_attack1_start_history_index(-1);
    if ((suppressed & (1ULL << 11)) != 0) pc.set_attack2_start_history_index(-1);

    if (moving)
    {
        if ((movement.forwardMove > 0.0F && (suppressed & kInForward) != 0) || (movement.forwardMove < 0.0F && (suppressed & kInBack) != 0))
            movement.forwardMove = 0.0F;
        if ((movement.leftMove > 0.0F && (suppressed & kInMoveLeft) != 0) || (movement.leftMove < 0.0F && (suppressed & kInMoveRight) != 0))
            movement.leftMove = 0.0F;
        base.set_forwardmove(movement.forwardMove);
        base.set_leftmove(movement.leftMove);
    }

    for (auto& step : *base.mutable_subtick_moves())
    {
        const uint64_t button = step.button();
        if ((button & forced.mask) != 0)
        {
            const uint64_t remaining = button & ~forced.mask;
            step.set_button(remaining);
            if (remaining == 0)
            {
                step.clear_pressed();
                step.clear_analog_forward_delta();
                step.clear_analog_left_delta();
            }
        }
        if (moving)
        {
            step.clear_analog_forward_delta();
            step.clear_analog_left_delta();
        }
    }
}

// Applies the recorded drop command.
void ApplyReplayDrop(int slot, void* services)
{
    ReplayDropEvent event{};
    if (!motion_recorder::TakeCurrentReplayDrop(slot, event)) return;
    motion_recorder::DropReplayEventWeapon(slot, services, event);
}

// Hooks commands from the live services vtable.
void EnsureVtableHooks(void* services);

// Discovers hooks and the recording's weapon services.
KHook::Return<void> HookedProcessMovement(void* services, void*) noexcept
{
    EnsureVtableHooks(services);
    if (!motion_recorder::HasAnyRecording()) return { KHook::Action::Ignore };
    int slot = pawn_binding::ServicesToSlot(services);
    if (motion_recorder::IsRecording(slot))
        motion_recorder::SetLiveWs(slot, pawn_binding::ServicesToWeaponServices(slot, services));
    return { KHook::Action::Ignore };
}

// Inactive nested batches shadow outer contexts.
CommandBatch* FindCommandBatch(int slot)
{
    for (auto it = g_commandBatches.rbegin(); it != g_commandBatches.rend(); ++it)
        if (it->slot == slot) return &*it;
    return nullptr;
}

// Finds the active frame for reentrancy checks.
CommandFrame* FindCommandFrame(int slot)
{
    for (auto it = g_commandFrames.rbegin(); it != g_commandFrames.rend(); ++it)
        if (it->slot == slot) return &*it;
    return nullptr;
}

// ---- PlayerRunCommand: subtick record + re-inject ----

// Captures command input and subticks.
void CaptureUserCommand(int slot, void* services, const PlayerCommand& pc, const CBaseUserCmdPB& base)
{
    const int n = base.subtick_moves_size();
    if (n > motion_recorder::kMaxSubtickPerTick)
    {
        BC_LOG_WARN("slot %d: subtick limit exceeded; recording stopped", slot);
        motion_recorder::StopRecord(slot);
        return;
    }
    std::string history;
    int simulationTick;
    if (!pawn_binding::CommandSimulationTick(slot, services, pc, simulationTick) || !command_history::Capture(pc, simulationTick, history))
    {
        BC_LOG_WARN("slot %d: invalid command history; recording stopped", slot);
        motion_recorder::StopRecord(slot);
        return;
    }
    // Stash subticks until this PlayerRunCommand returns.
    SubtickMove moves[motion_recorder::kMaxSubtickPerTick];
    for (int i = 0; i < n; ++i)
    {
        const CSubtickMoveStep& s = base.subtick_moves(i);
        moves[i].when = s.when();
        moves[i].button = s.button();
        moves[i].pressed = s.pressed() ? 1.0F : 0.0F;
        moves[i].analogForward = s.analog_forward_delta();
        moves[i].analogLeft = s.analog_left_delta();
        moves[i].pitchDelta = s.pitch_delta();
        moves[i].yawDelta = s.yaw_delta();
    }
    motion_recorder::OnCaptureSubticks(slot, moves, n);

    ReplayCommandFrameData command{};
    command.buttons = pc.buttonstates.m_pButtonStates[0];
    command.buttons1 = pc.buttonstates.m_pButtonStates[1];
    command.buttons2 = pc.buttonstates.m_pButtonStates[2];
    command.fields |= motion_recorder::kCommandFieldButtons;
    if (base.has_forwardmove())
    {
        command.forwardMove = base.forwardmove();
        command.fields |= motion_recorder::kCommandFieldForwardMove;
    }
    if (base.has_leftmove())
    {
        command.leftMove = base.leftmove();
        command.fields |= motion_recorder::kCommandFieldLeftMove;
    }
    if (base.has_upmove())
    {
        command.upMove = base.upmove();
        command.fields |= motion_recorder::kCommandFieldUpMove;
    }
    if (base.has_viewangles())
    {
        const CMsgQAngle& view = base.viewangles();
        command.pitch = view.x();
        command.yaw = view.y();
        command.roll = view.z();
        command.fields |= motion_recorder::kCommandFieldViewAngles;
    }
    if (base.has_mousedx() || base.has_mousedy())
    {
        command.mouseDx = base.mousedx();
        command.mouseDy = base.mousedy();
        command.fields |= motion_recorder::kCommandFieldMouse;
    }
    if (base.has_weaponselect())
    {
        command.weaponSelect =
            base.weaponselect() > 0
                ? weapon_locker_hooks::WeaponDefForEntityIndex(pawn_binding::ServicesToWeaponServices(slot, services), base.weaponselect())
                : 0;
        command.fields |= motion_recorder::kCommandFieldWeaponSelect;
    }
    if (pc.has_left_hand_desired())
    {
        command.leftHandDesired = pc.left_hand_desired() ? 1 : 0;
        command.fields |= motion_recorder::kCommandFieldLeftHand;
    }
    motion_recorder::OnCaptureCommand(slot, command, std::move(history));
}

// Applies replay input before simulation.
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

        // Restore recorded subtick moves.
        base->clear_subtick_moves();
        for (int i = 0; i < frame.subtickCount; ++i)
        {
            uint64_t button = frame.subticks[i].button;
            float pressed = frame.subticks[i].pressed;
            if (suppressUnsafeUtilityAttack && (button & kGrenadeAttackMask) != 0)
            {
                button &= ~kGrenadeAttackMask;
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
    if (!ValidSlotIndex(slot) || !cmd || FindCommandFrame(slot) || !ResolveReplayPawn(slot, services)) return { KHook::Action::Ignore };
    const uint64_t generation = motion_recorder::PawnGeneration(slot);
    auto* batch = FindCommandBatch(slot);
    if (batch && (batch->services != services || batch->generation != generation || batch->preparing)) return { KHook::Action::Ignore };
    const bool recording = batch && motion_recorder::IsRecording(slot);
    const bool replaying = batch && motion_recorder::IsReplaying(slot);
    const bool capturePrepared = batch && batch->capturePrepared;
    if (batch) batch->capturePrepared = false;
    const bool controls = !replaying && (g_usercmdWorkSlots.load(std::memory_order_acquire) & (uint64_t{ 1 } << slot)) != 0;

    if (recording || replaying || controls)
    {
        g_commandFrames[index] = { slot, services, generation, recording, false };
        // Compiler computes the multiple-inheritance adjust here.
        auto* pc = reinterpret_cast<PlayerCommand*>(cmd);
        CBaseUserCmdPB* base = pc->mutable_base();

        if (recording && !capturePrepared) motion_recorder::OnRecordCommandPre(slot, services);

        if (replaying)
        {
            const bool applied = ApplyReplayUserCommand(slot, services, pc, base);
            g_commandFrames[index].replayApplied = applied;
        }
        else
        {
            if (controls)
            {
                uint64_t previousHeld = 0;
                if (!SafeRead(services, tg::g_servicesButtons, previousHeld))
                {
                    BC_LOG_WARN("slot %d: button state unavailable; session stopped", slot);
                    motion_recorder::InvalidateSlot(slot);
                    return { KHook::Action::Ignore };
                }
                ApplyUsercmdControls(slot, *pc, *base, previousHeld);
            }
            if (recording) CaptureUserCommand(slot, services, *pc, *base);
        }
    }

    return { KHook::Action::Ignore };
}

// Commits one command under the reentrancy guard.
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

// Applies queued drops after native command setup.
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

// Hooks controller command setup.
void EnsureControllerCommandHook(void* controller)
{
    if (!controller || g_controllerHookTried.exchange(true, std::memory_order_acq_rel)) return;
    void** vtable = nullptr;
    void* target = nullptr;
    if (GuardedRead(controller, 0, vtable) && vtable &&
        GuardedRead(static_cast<const void*>(vtable), tg::g_vtIdxControllerCommandSetup * static_cast<int>(sizeof(void*)), target) &&
        target && g_hookControllerCommandSetup.Install(target, nullptr, &ControllerCommandSetupPost))
        return;
    g_hookControllerCommandSetup.Remove();
    BC_LOG_WARN("record/replay: command setup unavailable");
}

// Tracks batch ownership; frames advance per command.
KHook::Return<void> SimulateUserCommandsPre(void* controller) noexcept
{
    EnsureControllerCommandHook(controller);
    if (!motion_recorder::HasAnyRecording() && !motion_recorder::HasAnyReplay())
    {
        // Pair the post callback with an inactive batch.
        g_commandBatches.push_back({ -1, nullptr });
        return { KHook::Action::Ignore };
    }
    int slot = ControllerToSlot(controller);
    if (!ValidSlotIndex(slot))
    {
        g_commandBatches.push_back({ -1, nullptr });
        return { KHook::Action::Ignore };
    }

    void* services = pawn_binding::ServicesForSlot(slot);

    // Guard nested batches for this slot.
    if (FindCommandBatch(slot))
    {
        g_commandBatches.push_back({ slot, nullptr });
        return { KHook::Action::Ignore };
    }
    // Publish ownership before native callbacks.
    g_commandBatches.push_back({ slot, services, motion_recorder::PawnGeneration(slot) });

    return { KHook::Action::Ignore };
}

// Empty batches do not advance frames.
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

    // A batch may contain multiple commands.
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

    // Hook PlayerRunCommand from the first live services.
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
        g_controlledButtons.fill(0);
        g_usercmdWorkSlots.store(0, std::memory_order_release);
    }
    g_installed = false;
    g_status = "not_attempted";
}

// Checks batch, command, and command-setup hooks.
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
