#include "core/gameconfig.h"
#include "core/log.h"
// CS2 movement hooks
// PhysicsSimulate (record/replay frame boundary)
// ProcessMovement (live services discovery)
// PlayerRunCommand(subtick record + re-inject)

#include "networkbasetypes.pb.h"
#include "nlohmann/json.hpp" // NOLINT(misc-include-cleaner)
#include "playercommand.h"

#include "InputInjector.h"
#include "PawnBinding.h"
#include "ccsbot_slot.h"
#include "core/memory_module.h"
#include "MotionRecorder.h"
#include "usercmd.pb.h"
#include "offsets.h"
#include "hooks.h"
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
void* g_addrPhysicsSimulate = nullptr;

hooks::NativeHook<void, void*, void*> g_hookProcessMovement;
hooks::NativeHook<void, void*, void*> g_hookPlayerRunCommand;
hooks::NativeHook<void, void*> g_hookPhysicsSimulate;
hooks::NativeHook<void, void*> g_hookControllerCommandSetup;
std::atomic<bool> g_controllerHookTried{ false };

struct MovementFrame
{
    int slot;
    void* services;
    bool recording;
    bool replaying;
    bool seeded = false;
};
thread_local std::vector<MovementFrame> g_physicsFrames;
bool g_installed = false;
// True once PhysicsSimulate is hooked
bool g_physicsActive = false;
// True once PlayerRunCommand is hooked
bool g_subtickActive = false;
std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

// slot -> live CCSPlayer_MovementServices*
std::array<std::atomic<void*>, kMaxSlots> g_slotServices{};

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

bool IsThrowableUtilityDef(int def) { return def >= 43 && def <= 48; }

// Reports whether a slot can index the fixed replay state arrays.
bool ValidSlotIndex(int slot) { return slot >= 0 && slot < kMaxSlots; }

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
    if (!ValidSlotIndex(slot) || buttonMask == 0 || durationMs < 0 || !g_subtickActive || motion_recorder::IsReplaying(slot)) return -1;

    int64_t id = g_nextUsercmdInjectionId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdInjections[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .durationMs = durationMs, .phase = UsercmdInjectionPhase::PendingPress });
    return id;
}

// Creates an independently cancellable persistent analog movement override
int64_t StartUsercmdMovement(int slot, float forwardMove, float leftMove)
{
    if (!ValidSlotIndex(slot) || !std::isfinite(forwardMove) || !std::isfinite(leftMove) || !g_subtickActive ||
        motion_recorder::IsReplaying(slot))
        return -1;

    int64_t id = g_nextUsercmdMovementId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdMovements[slot].push_back(
        { .id = id, .forwardMove = std::clamp(forwardMove, -1.0F, 1.0F), .leftMove = std::clamp(leftMove, -1.0F, 1.0F) });
    return id;
}

// Updates one persistent analog movement override
bool UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove)
{
    if (!ValidSlotIndex(slot) || movementId <= 0 || !std::isfinite(forwardMove) || !std::isfinite(leftMove)) return false;

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
    if (!ValidSlotIndex(slot) || buttonMask == 0 || durationMs <= 0 || !g_subtickActive || motion_recorder::IsReplaying(slot)) return false;

    int64_t expiresAtMs = MonotonicMilliseconds() + durationMs;
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return false;
    g_usercmdSuppressions[slot].push_back(
        { .id = 0, .buttonMask = buttonMask, .expiresAtMs = expiresAtMs, .persistent = false, .releasePending = true });
    return true;
}

// Creates an independently cancellable persistent usercmd suppression
int64_t StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || !g_subtickActive || motion_recorder::IsReplaying(slot)) return -1;

    int64_t id = g_nextUsercmdSuppressionId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
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

// Discovers live services without modifying any substep simulation state.
KHook::Return<void> HookedProcessMovement(void* services, void*) noexcept
{
    EnsureVtableHooks(services);
    void* validatedPawn = nullptr;
    int slot = pawn_binding::ServicesToSlot(services, &validatedPawn);
    if (slot >= 0 && slot < kMaxSlots)
    {
        g_slotServices[slot].store(services, std::memory_order_release);
        if (motion_recorder::IsRecording(slot))
            motion_recorder::SetLiveWs(slot, pawn_binding::ServicesToWeaponServices(slot, services, validatedPawn));
    }
    return { KHook::Action::Ignore };
}

// Returns the enclosing simulation boundary for this player.
MovementFrame* FindPhysicsFrame(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    for (auto it = g_physicsFrames.rbegin(); it != g_physicsFrames.rend(); ++it)
        if (it->slot == slot) return &*it;
    return nullptr;
}

// ---- PlayerRunCommand: subtick record + re-inject ----

// Serializes command fields and subticks into the pending recording frame.
void CaptureUserCommand(int slot, void* services, PlayerCommand* pc, CBaseUserCmdPB* base)
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
                                         pawn_binding::ServicesToWeaponServices(slot, services, nullptr), base->weaponselect())
                                   : 0;
        command.fields |= motion_recorder::kCommandFieldWeaponSelect;
    }
    if (pc->has_left_hand_desired())
    {
        command.leftHandDesired = pc->left_hand_desired() ? 1 : 0;
        command.fields |= motion_recorder::kCommandFieldLeftHand;
    }
    motion_recorder::OnCaptureCommand(slot, command);
}

// Applies one recorded command before the engine simulates it.
void ApplyReplayUserCommand(int slot, void* services, PlayerCommand* pc, CBaseUserCmdPB* base)
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

        // Mark before calling the engine: reentrant commands must not reseed.
        auto* boundary = FindPhysicsFrame(slot);
        if (boundary && !boundary->seeded)
        {
            boundary->seeded = true;
            motion_recorder::OnReplayCommandPre(slot, services, frame.tick);
        }
    }
}

// Records or injects the user command before native simulation.
KHook::Return<void> HookedPlayerRunCommand(void* services, void* cmd) noexcept
{
    int slot = pawn_binding::ServicesToSlot(services);
    auto* boundary = FindPhysicsFrame(slot);
    bool recording = boundary && boundary->recording && motion_recorder::IsRecording(slot);
    bool replaying = boundary && boundary->replaying && motion_recorder::IsReplaying(slot);
    const auto [hasUsercmdInjection, hasUsercmdSuppression, hasUsercmdMovement] = replaying ? UsercmdWork{} : GetUsercmdWork(slot);

    if (cmd && (recording || replaying || hasUsercmdInjection || hasUsercmdSuppression || hasUsercmdMovement))
    {
        // Compiler computes the multiple-inheritance adjust here.
        auto* pc = reinterpret_cast<PlayerCommand*>(cmd);
        CBaseUserCmdPB* base = pc->mutable_base();

        if (recording) CaptureUserCommand(slot, services, pc, base);

        if (replaying) ApplyReplayUserCommand(slot, services, pc, base);

        if (hasUsercmdSuppression && !replaying) ApplyUsercmdSuppressions(slot, pc, base);
        if (hasUsercmdInjection && !replaying) ApplyUsercmdInjections(slot, pc, base);
        if (hasUsercmdMovement && !replaying) ApplyUsercmdMovement(slot, pc, base);
    }

    return { KHook::Action::Ignore };
}

// ---- PhysicsSimulate: the per-tick boundary ----
// Records pre/post + commits

// Dispatches drops after native command setup, at the client-command boundary.
KHook::Return<void> ControllerCommandSetupPost(void* controller) noexcept
{
    if (g_physicsFrames.empty()) return { KHook::Action::Ignore };
    // Do not cross an inactive/nested simulation frame to consume an outer event.
    const MovementFrame frame = g_physicsFrames.back();
    if (frame.replaying && frame.services && ControllerToSlot(controller) == frame.slot && motion_recorder::IsReplaying(frame.slot))
        ApplyReplayDrop(frame.slot, frame.services);
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
    BC_LOG_WARN("Controller command setup hook unavailable; recording and replay are disabled\n");
}

// Captures the per-tick state before any subtick movement.
KHook::Return<void> HookedPhysicsSimulate(void* controller) noexcept
{
    EnsureControllerCommandHook(controller);
    if (!motion_recorder::HasAnyRecording() && !motion_recorder::HasAnyReplay())
    {
        // Keep the matching post callback from consuming an outer frame.
        g_physicsFrames.push_back({ -1, nullptr, false, false });
        return { KHook::Action::Ignore };
    }
    int slot = ControllerToSlot(controller);

    void* services = (slot >= 0 && slot < kMaxSlots) ? g_slotServices[slot].load(std::memory_order_acquire) : nullptr;

    bool recording = slot >= 0 && slot < kMaxSlots && services && motion_recorder::IsRecording(slot);
    bool replaying = slot >= 0 && slot < kMaxSlots && services && motion_recorder::IsReplaying(slot);

    // A nested simulation for this slot belongs to the existing outer boundary.
    if (FindPhysicsFrame(slot))
    {
        g_physicsFrames.push_back({ -1, nullptr, false, false });
        return { KHook::Action::Ignore };
    }
    // Publish ownership before engine callbacks can reenter.
    g_physicsFrames.push_back({ slot, services, recording, replaying });

    // pre: snapshot start-of-tick state once (before any subtick mover).
    if (recording) motion_recorder::OnCapturePre(slot, services, nullptr);

    return { KHook::Action::Ignore };
}

// Commits the recording and replay state for the matching simulation call.
KHook::Return<void> PhysicsSimulatePost(void*) noexcept
{
    const auto [slot, services, recording, replaying, seeded] = g_physicsFrames.back();
    g_physicsFrames.pop_back();

    // post: snapshot end-of-tick state + commit one frame
    if (recording) motion_recorder::OnCapturePost(slot, services, nullptr);
    if (replaying)
    {
        motion_recorder::OnReplayCommit(slot, services, seeded);
    }
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
    if (g_addrPlayerRunCommand && g_hookPlayerRunCommand.Install(g_addrPlayerRunCommand, &HookedPlayerRunCommand))
    {
        g_subtickActive = true;
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
    const modules::ModuleInfo& serverModule,
    char* errorOut,
    size_t errorOutLen) // NOLINT(misc-use-internal-linkage)
{
    g_addrProcessMovement = gameconfig::ResolveSig(gd, serverModule, "CCSPlayer_MovementServices::ProcessMovement", errorOut, errorOutLen);
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

    // PhysicsSimulate: the per-tick boundary
    char psErr[256] = { 0 };
    g_addrPhysicsSimulate = gameconfig::ResolveSig(gd, serverModule, "CBasePlayerController::OnSimulateUserCommands", psErr, sizeof(psErr));
    if (g_addrPhysicsSimulate && g_hookPhysicsSimulate.Install(g_addrPhysicsSimulate, &HookedPhysicsSimulate, &PhysicsSimulatePost))
    {
        g_physicsActive = true;
    }
    else
    {
        if (g_addrPhysicsSimulate)
        {
            g_hookPhysicsSimulate.Remove();
            g_addrPhysicsSimulate = nullptr;
        }
        BC_LOG_WARN("PhysicsSimulate hook unavailable (%s); recording and replay are disabled\n", psErr[0] ? psErr : "KHook failed");
    }

    // PlayerRunCommand is hooked lazily from the first live movement-services vtable.
    char dropReleaseError[256]{};
    void* outerDrop = gameconfig::ResolveSig(gd, serverModule, "DropReleasePose::OuterDrop", dropReleaseError, sizeof(dropReleaseError));
    void* buildTransform =
        outerDrop ? gameconfig::ResolveSig(gd, serverModule, "DropReleasePose::BuildTransform", dropReleaseError, sizeof(dropReleaseError))
                  : nullptr;
    if (!motion_recorder::InstallDropReleasePose(outerDrop, buildTransform))
        BC_LOG_WARN("Drop release pose unavailable (%s); drop replay requires this hook\n",
                    dropReleaseError[0] ? dropReleaseError : "hook installation failed");
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
    g_hookPhysicsSimulate.Remove();
    g_addrProcessMovement = nullptr;
    g_addrPlayerRunCommand = nullptr;
    g_addrPhysicsSimulate = nullptr;
    g_physicsActive = false;
    g_subtickActive = false;
    g_vtHooksTried.store(false, std::memory_order_release);
    g_controllerHookTried.store(false, std::memory_order_release);
    for (auto& s : g_slotServices)
        s.store(nullptr, std::memory_order_release);
    pawn_binding::ClearAll();
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
    g_installed = false;
    g_status = "not_attempted";
}

// Recording and replay require frame, client-command, and movement boundaries.
bool RecorderReady()
{
    if (g_physicsActive && g_subtickActive && g_hookControllerCommandSetup.Active()) return true;
    BC_LOG_WARN("Cannot start recording/replay: PhysicsSimulate=%d PlayerRunCommand=%d ControllerCommandSetup=%d\n", g_physicsActive,
                g_subtickActive, g_hookControllerCommandSetup.Active());
    return false;
}

const char* Status() { return g_status.c_str(); }

} // namespace input_injector
} // namespace cs2bc
