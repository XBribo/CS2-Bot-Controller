// Motion recording & replay implementation

#include "MotionRecorder.h"
#include "BotController.h"
#include "dispatch.h"
#include "InputInjector.h"
#include "WeaponLocker.h"
#include "ccsbot_slot.h"
#include "hooks.h"
#include "version_targets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector> // NOLINT(misc-include-cleaner)

#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

namespace tg = cs2bc::targets;

namespace cs2bc {
namespace motion_recorder {
#ifdef _WIN32
using DropWeaponResult = uint8_t;
#else
using DropWeaponResult = void*;
#endif
using DropWeaponT = DropWeaponResult(BC_FASTCALL*)(void* weaponServices, void* weapon, void* target, void* velocity);

struct RecordState
{
    struct DropCandidate
    {
        void* weapon;
        ReplayDropEvent event;
    };

    std::atomic<bool> recording{ false };
    std::vector<ReplayTick> ticks;
    std::vector<SubtickMove> subs;
    std::vector<ReplayCommandFrameData> commands;
    // Subtick moves seen on PlayerRunCommand, awaiting the matching
    // ProcessMovement post that commits them to a tick.
    std::vector<SubtickMove> pendingSubs;
    ReplayCommandFrameData pendingCommand{};
    bool havePendingCommand{ false };
    MovementSnapshot pendingPre{};
    bool havePre{ false };
    uint32_t pendingEventFlags{ ReplayEventNone };
    ReplayDropEvent pendingDropEvent{ .weaponDefIndex = -1, .vectorFlags = ReplayDropVectorNone, .target = {}, .velocity = {} };
    std::vector<DropCandidate> pendingDropCandidates;
    std::atomic<void*> liveWs{ nullptr };
    std::atomic<int> currentDef{ -1 };
    std::mutex mu; // guards ticks/subs/pending/pre
};

struct ReplayState
{
    std::atomic<bool> playing{ false };
    std::atomic<bool> loop{ false };
    std::vector<ReplayTick> ticks;
    std::vector<SubtickMove> subs;
    std::vector<ReplayCommandFrameData> commands;
    std::vector<ReplayMovementExtra> movementExtras;
    std::vector<uint32_t> subOffset; // prefix sum, size ticks.size()+1
    std::atomic<int> cursor{ 0 };
    std::atomic<int> lastAppliedDef{ -1 };
    int lastEventCursor{ -1 };
    std::mutex mu; // guards ticks/subs/subOffset
};

namespace {

std::array<RecordState, kMaxSlots> g_rec;
std::array<ReplayState, kMaxSlots> g_rep;
std::atomic<uint64_t> g_dropHookCallCount{ 0 };
std::atomic<uint64_t> g_dropHookRecordingCallCount{ 0 };
std::atomic<uint64_t> g_dropHookPhysicalDropCount{ 0 };
std::atomic<uint64_t> g_dropHookInvalidDefCount{ 0 };
std::atomic<uint64_t> g_dropCaptureCount{ 0 };
std::atomic<uint64_t> g_dropReplayAttemptCount{ 0 };
std::atomic<uint64_t> g_dropReplayHookCallCount{ 0 };
std::atomic<uint64_t> g_dropReplayVectorOverrideCount{ 0 };
std::atomic<uint64_t> g_dropReplayDetachedCount{ 0 };
std::atomic<uint64_t> g_dropReplayNativeCallCount{ 0 };
std::atomic<int> g_lastDropCaptureSlot{ -1 };
std::atomic<uint32_t> g_lastDropCaptureVectorFlags{ ReplayDropVectorNone };
std::atomic<int> g_lastDropHookSlot{ -1 };
std::atomic<int> g_lastDropHookWeaponDef{ -1 };
std::atomic<bool> g_lastDropHookWasRecording{ false };
std::atomic<void*> g_lastDropHookPawn{ nullptr };
std::atomic<void*> g_lastDropHookTarget{ nullptr };
std::atomic<void*> g_lastDropHookVelocity{ nullptr };
std::atomic<int> g_lastDropReplaySlot{ -1 };
std::atomic<int> g_lastDropReplayWeaponDef{ -1 };
std::atomic<uint32_t> g_lastDropReplayVectorFlags{ ReplayDropVectorNone };
hooks::NativeHook<DropWeaponResult, void*, void*, void*, void*> g_hookDropWeapon;
struct DropFrame
{
    void* weaponServices;
    void* weapon;
    int recordingSlot;
    int weaponDefIndex;
    ReplayDropEvent recordedEvent;
};
thread_local std::vector<DropFrame> g_dropFrames;
void* g_addrDropWeapon = nullptr;
std::atomic<bool> g_dropHookTried{ false };
std::atomic<bool> g_dropHookReady{ false };
thread_local int g_activeReplayDropSlot = -1;
thread_local const ReplayDropEvent* g_activeReplayDropEvent = nullptr;
constexpr int kMolotovDef = 46;
constexpr int kIncendiaryDef = 48;

bool ValidSlot(int s) { return s >= 0 && s < kMaxSlots; }

static int ReplayWeaponSelectForDef(int slot, int recordedDef);

// Returns whether an item definition is either faction's fire grenade
bool IsFireGrenadeDef(int defIndex) { return defIndex == kMolotovDef || defIndex == kIncendiaryDef; }

// Prefers the recorded fire grenade and falls back to the other faction's variant
void* FindReplayWeaponByDef(void* ws, int recordedDef)
{
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, recordedDef);
    if (weapon || !IsFireGrenadeDef(recordedDef)) return weapon;

    const int alternateDef = recordedDef == kMolotovDef ? kIncendiaryDef : kMolotovDef;
    return weapon_locker_hooks::FindWeaponByDef(ws, alternateDef);
}

// Copies an optional engine Vector into stable recording storage
bool ReadDropVector(void* vector, float out[3]) { return vector && TryReadMemory(vector, 0, out, sizeof(float) * 3); }

// Prefers the recorder's exact cached weapon-services owner over controller handles
int RecordingSlotForWeaponServices(void* weaponServices, void* pawn)
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        RecordState& r = g_rec[slot];
        if (r.recording.load(std::memory_order_acquire) && r.liveWs.load(std::memory_order_relaxed) == weaponServices) return slot;
    }

    const int slot = ControllerSlotForPawn(pawn);
    return IsRecording(slot) ? slot : -1;
}

// Resolves the replay slot that currently owns this weapon-services pointer
int ReplaySlotForWeaponServices(void* weaponServices)
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        if (bot_controller_hooks::IsLiveBotSlot(slot) && IsReplaying(slot) && weapon_locker_hooks::WsForSlot(slot) == weaponServices)
            return slot;
    }
    return -1;
}

// Stores one real weapon-service drop in the pending recording tick
bool CaptureDropEvent(int slot, const ReplayDropEvent& event)
{
    if (!ValidSlot(slot) || event.weaponDefIndex < 0) return false;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return false;

    std::scoped_lock lk(r.mu);
    r.pendingEventFlags |= ReplayEventDrop;
    r.pendingDropEvent = event;
    g_dropCaptureCount.fetch_add(1, std::memory_order_relaxed);
    g_lastDropCaptureSlot.store(slot, std::memory_order_relaxed);
    g_lastDropCaptureVectorFlags.store(event.vectorFlags, std::memory_order_relaxed);
    return true;
}

// Captures drop inputs and recalls the remaining hooks when replay changes vectors.
KHook::Return<DropWeaponResult> HookedDropWeapon(void* weaponServices, void* weapon, void* target, void* velocity) noexcept
{
    g_dropHookCallCount.fetch_add(1, std::memory_order_relaxed);

    void* pawn = nullptr;
    if (weaponServices) GuardedRead(weaponServices, tg::g_servicesPawn, pawn);
    const int recordingSlot = RecordingSlotForWeaponServices(weaponServices, pawn);
    const int activeReplaySlot =
        ValidSlot(g_activeReplayDropSlot) && bot_controller_hooks::IsLiveBotSlot(g_activeReplayDropSlot) ? g_activeReplayDropSlot : -1;
    const int replaySlot = ValidSlot(activeReplaySlot) ? activeReplaySlot : ReplaySlotForWeaponServices(weaponServices);
    const int slot = ValidSlot(recordingSlot) ? recordingSlot : replaySlot;

    // The DropWeapon vtable hook is global once installed for recording, so it
    // can observe unrelated players. If this call belongs to neither an active
    // recorder nor a replaying bot, keep the paired post-hook balanced but do
    // no weapon/vector work.
    if (!ValidSlot(recordingSlot) && !ValidSlot(replaySlot))
    {
        g_dropFrames.push_back({ nullptr, nullptr, -1, -1, {} });
        return { KHook::Action::Ignore };
    }

    int weaponDefIndex = weapon_locker_hooks::ReadDefIndex(weapon);
    if (weaponDefIndex < 0 && weaponServices) weaponDefIndex = weapon_locker_hooks::ActiveWeaponDef(weaponServices);
    if (weaponDefIndex < 0 && ValidSlot(recordingSlot)) weaponDefIndex = g_rec[recordingSlot].currentDef.load(std::memory_order_relaxed);

    ReplayDropEvent recordedEvent{};
    recordedEvent.weaponDefIndex = weaponDefIndex;
    if (ReadDropVector(target, recordedEvent.target)) recordedEvent.vectorFlags |= ReplayDropVectorTarget;
    if (ReadDropVector(velocity, recordedEvent.velocity)) recordedEvent.vectorFlags |= ReplayDropVectorVelocity;

    float replayTarget[3] = {};
    float replayVelocity[3] = {};
    void* effectiveTarget = target;
    void* effectiveVelocity = velocity;
    if (ValidSlot(activeReplaySlot) && g_activeReplayDropEvent)
    {
        if (g_activeReplayDropEvent->vectorFlags != ReplayDropVectorNone)
            g_dropReplayVectorOverrideCount.fetch_add(1, std::memory_order_relaxed);
        if ((g_activeReplayDropEvent->vectorFlags & ReplayDropVectorTarget) != 0)
        {
            for (int i = 0; i < 3; ++i)
                replayTarget[i] = g_activeReplayDropEvent->target[i];
            effectiveTarget = replayTarget;
        }
        if ((g_activeReplayDropEvent->vectorFlags & ReplayDropVectorVelocity) != 0)
        {
            for (int i = 0; i < 3; ++i)
                replayVelocity[i] = g_activeReplayDropEvent->velocity[i];
            effectiveVelocity = replayVelocity;
        }
    }

    if (ValidSlot(recordingSlot))
    {
        g_dropHookRecordingCallCount.fetch_add(1, std::memory_order_relaxed);
        if (weaponDefIndex < 0) g_dropHookInvalidDefCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (ValidSlot(activeReplaySlot)) g_dropReplayHookCallCount.fetch_add(1, std::memory_order_relaxed);

    g_lastDropHookPawn.store(pawn, std::memory_order_relaxed);
    g_lastDropHookSlot.store(slot, std::memory_order_relaxed);
    g_lastDropHookWeaponDef.store(weaponDefIndex, std::memory_order_relaxed);
    g_lastDropHookWasRecording.store(ValidSlot(recordingSlot), std::memory_order_relaxed);
    g_lastDropHookTarget.store(effectiveTarget, std::memory_order_relaxed);
    g_lastDropHookVelocity.store(effectiveVelocity, std::memory_order_relaxed);

    g_dropFrames.push_back({ weaponServices, weapon, recordingSlot, weaponDefIndex, recordedEvent });
    if (effectiveTarget != target || effectiveVelocity != velocity)
        return KHook::Recall(reinterpret_cast<DropWeaponT>(g_addrDropWeapon), KHook::Return<DropWeaponResult>{ KHook::Action::Ignore },
                             weaponServices, weapon, effectiveTarget, effectiveVelocity);
    return { KHook::Action::Ignore };
}

// Records only physical detachments and preserves the engine's return value.
KHook::Return<DropWeaponResult> DropWeaponPost(void*, void*, void*, void*) noexcept
{
    // Defensive guard: a mismatched pre/post callback must never crash the
    // server by reading an empty thread-local frame stack.
    if (g_dropFrames.empty()) return { KHook::Action::Ignore };

    const auto [weaponServices, weapon, recordingSlot, weaponDefIndex, recordedEvent] = g_dropFrames.back();
    g_dropFrames.pop_back();
    const bool detached =
        weaponServices && weapon && weaponDefIndex >= 0 && weapon_locker_hooks::FindWeaponByDef(weaponServices, weaponDefIndex) != weapon;
    if (detached && ValidSlot(recordingSlot))
    {
        g_dropHookPhysicalDropCount.fetch_add(1, std::memory_order_relaxed);
        CaptureDropEvent(recordingSlot, recordedEvent);
    }
    else if (ValidSlot(recordingSlot) && weapon && weaponDefIndex >= 0)
    {
        RecordState& r = g_rec[recordingSlot];
        std::scoped_lock lk(r.mu);
        bool found = false;
        for (RecordState::DropCandidate& candidate : r.pendingDropCandidates)
        {
            if (candidate.weapon == weapon)
            {
                candidate.event = recordedEvent;
                found = true;
                break;
            }
        }
        if (!found) r.pendingDropCandidates.push_back({ .weapon = weapon, .event = recordedEvent });
    }
    if (detached && ValidSlot(g_activeReplayDropSlot) && bot_controller_hooks::IsLiveBotSlot(g_activeReplayDropSlot))
        g_dropReplayDetachedCount.fetch_add(1, std::memory_order_relaxed);
    return { KHook::Action::Ignore };
}

// Installs the drop hook from a live weapon-services vtable once.
//
// Do not mark the lazy lookup as attempted until a valid vtable entry has
// actually been resolved. Otherwise one transient bad services pointer would
// permanently disable drop capture until the next runtime reset.
void EnsureDropWeaponHook(void* weaponServices)
{
    if (!weaponServices || g_dropHookReady.load(std::memory_order_acquire)) return;

    void** vtable = nullptr;
    if (!GuardedRead(weaponServices, 0, vtable) || !vtable) return;

    void* resolvedDropWeapon = nullptr;
    if (!GuardedRead(static_cast<const void*>(vtable), tg::g_vtIdxDropWeapon * static_cast<int>(sizeof(void*)), resolvedDropWeapon) ||
        !resolvedDropWeapon)
    {
        return;
    }

    bool expected = false;
    if (!g_dropHookTried.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    g_addrDropWeapon = resolvedDropWeapon;

    if (g_hookDropWeapon.Install(g_addrDropWeapon, &HookedDropWeapon, &DropWeaponPost))
    {
        g_dropHookReady.store(true, std::memory_order_release);
        return;
    }

    // A real install attempt failed. Keep g_dropHookTried=true for this
    // runtime cycle so SetLiveWs() does not retry an expensive failed hook on
    // every movement tick. ClearAll() resets the lazy state for the next cycle.
    g_hookDropWeapon.Remove();
    g_addrDropWeapon = nullptr;
}

// Reads a three-float engine vector through one guarded memory operation.
bool ReadVector3(void* base, int offset, float& x, float& y, float& z)
{
    float values[3] = {};
    if (!TryReadMemory(base, offset, values, sizeof(values))) return false;
    x = values[0];
    y = values[1];
    z = values[2];
    return true;
}

// Writes a three-float engine vector through one guarded memory operation.
bool WriteVector3(void* base, int offset, float x, float y, float z)
{
    const float values[3] = { x, y, z };
    return TryWriteMemory(base, offset, values, sizeof(values));
}

// Resolves the current scene node through the July 2026 body component layout.
void* ResolveSceneNode(void* entity)
{
    void* body = nullptr;
    if (!GuardedRead(entity, tg::g_entBodyComponent, body) || !body) return nullptr;

    void* node = nullptr;
    return GuardedRead(body, tg::g_bodySceneNode, node) ? node : nullptr;
}

// Read a MovementSnapshot from live engine state (services -> pawn).
bool ReadSnapshot(int slot, void* services, MovementSnapshot& out)
{
    if (!services) return false;
    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn) return false;

    void* node = ResolveSceneNode(pawn);
    return node && ReadVector3(pawn, tg::g_entAbsVelocity, out.velX, out.velY, out.velZ) &&
           SafeRead(pawn, tg::g_entFlags, out.entityFlags) && SafeRead(pawn, tg::g_entMoveType, out.moveType) &&
           SafeRead(pawn, tg::g_entActualMoveType, out.actualMoveType) && SafeRead(services, tg::g_servicesButtons, out.buttons) &&
           SafeRead(services, tg::g_servicesButtons1, out.buttons1) && SafeRead(services, tg::g_servicesButtons2, out.buttons2) &&
           SafeRead(services, tg::g_servicesDuckAmount, out.duckAmount) && SafeRead(services, tg::g_servicesDuckSpeed, out.duckSpeed) &&
           ReadVector3(services, tg::g_servicesLadderNormal, out.ladderNormalX, out.ladderNormalY, out.ladderNormalZ) &&
           SafeRead(services, tg::g_servicesDucked, out.ducked) && SafeRead(services, tg::g_servicesDucking, out.ducking) &&
           SafeRead(services, tg::g_servicesDesiresDuck, out.desiresDuck) &&
           ReadVector3(pawn, tg::g_pawnViewAngle, out.pitch, out.yaw, out.roll) &&
           ReadVector3(node, tg::g_nodeAbsOrigin, out.originX, out.originY, out.originZ);
}

// ---- recording ----

} // namespace

bool StartRecord(int slot)
{
    if (!ValidSlot(slot) || IsReplaying(slot)) return false;

    RecordState& r = g_rec[slot];
    {
        std::scoped_lock lk(r.mu);
        r.ticks.clear();
        r.subs.clear();
        r.commands.clear();
        r.pendingSubs.clear();
        r.pendingCommand = {};
        r.havePendingCommand = false;
        r.havePre = false;
        r.pendingEventFlags = ReplayEventNone;
        r.pendingDropEvent = {};
        r.pendingDropEvent.weaponDefIndex = -1;
        r.pendingDropCandidates.clear();
        r.ticks.reserve(4096); // ~64s @ 64 tick
        r.subs.reserve(4096);
        r.commands.reserve(4096);
    }
    r.currentDef.store(-1, std::memory_order_relaxed);
    r.liveWs.store(nullptr, std::memory_order_relaxed);
    r.recording.store(true, std::memory_order_release);
    return true;
}

bool StopRecord(int slot)
{
    if (!ValidSlot(slot)) return false;
    g_rec[slot].recording.store(false, std::memory_order_release);
    return true;
}

bool IsRecording(int slot) { return ValidSlot(slot) && g_rec[slot].recording.load(std::memory_order_acquire); }

int RecordedTickCount(int slot)
{
    if (!ValidSlot(slot)) return -1;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    return static_cast<int>(r.ticks.size());
}

int RecordedSubtickCount(int slot)
{
    if (!ValidSlot(slot)) return -1;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    return static_cast<int>(r.subs.size());
}

// Returns the number of complete command frames captured for a slot
int RecordedCommandCount(int slot)
{
    if (!ValidSlot(slot)) return -1;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    return static_cast<int>(r.commands.size());
}

void SetLiveWs(int slot, void* ws)
{
    if (!ValidSlot(slot)) return;

    RecordState& r = g_rec[slot];
    r.liveWs.store(ws, std::memory_order_relaxed);

    // DropWeapon interception is needed only while a recording is active.
    // Replay uses the already-installed hook when a recorded drop must be
    // reproduced; ordinary players should never cause lazy hook installation.
    if (ws && r.recording.load(std::memory_order_acquire)) EnsureDropWeaponHook(ws);
}

void* LiveWs(int slot) { return ValidSlot(slot) ? g_rec[slot].liveWs.load(std::memory_order_relaxed) : nullptr; }

void SetCurrentDef(int slot, int defIndex)
{
    if (ValidSlot(slot)) g_rec[slot].currentDef.store(defIndex, std::memory_order_relaxed);
}

void OnCapturePre(int slot, void* services, void* cmd)
{
    (void)cmd;
    if (!ValidSlot(slot) || !services) return;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return;
    MovementSnapshot pre{};
    if (!ReadSnapshot(slot, services, pre)) return;
    std::scoped_lock lk(r.mu);
    r.pendingPre = pre;
    r.havePre = true;
}

void OnCaptureSubticks(int slot, const SubtickMove* moves, int count)
{
    if (!ValidSlot(slot) || count < 0) return;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return;
    count = std::min(count, kMaxSubtickPerTick);
    std::scoped_lock lk(r.mu);
    r.pendingSubs.clear();
    for (int i = 0; i < count; ++i)
        r.pendingSubs.push_back(moves[i]);
}

// Stashes the command frame until the matching movement tick is committed
void OnCaptureCommand(int slot, const ReplayCommandFrameData& command)
{
    if (!ValidSlot(slot)) return;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return;
    std::scoped_lock lk(r.mu);
    r.pendingCommand = command;
    r.havePendingCommand = true;
}

void OnCapturePost(int slot, void* services, void* cmd)
{
    // cmd is actually the CMoveData* (hook passes moveData here)
    if (!ValidSlot(slot) || !services) return;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return;

    MovementSnapshot post{};
    if (!ReadSnapshot(slot, services, post)) return;

    if (cmd)
    {
        ReadVector3(cmd, tg::g_moveAbsOrigin, post.originX, post.originY, post.originZ);
    }

    // Active weapon def for this tick.
    void* ws = r.liveWs.load(std::memory_order_relaxed);
    int def = weapon_locker_hooks::ActiveWeaponDef(ws);
    if (def < 0) def = r.currentDef.load(std::memory_order_relaxed);
    if (def >= 0) r.currentDef.store(def, std::memory_order_relaxed);

    uint32_t subtickCount;
    {
        std::scoped_lock lk(r.mu);
        for (size_t i = 0; i < r.pendingDropCandidates.size();)
        {
            const RecordState::DropCandidate& candidate = r.pendingDropCandidates[i];
            if (weapon_locker_hooks::FindWeaponByDef(ws, candidate.event.weaponDefIndex) != candidate.weapon)
            {
                r.pendingEventFlags |= ReplayEventDrop;
                r.pendingDropEvent = candidate.event;
                g_dropHookPhysicalDropCount.fetch_add(1, std::memory_order_relaxed);
                g_dropCaptureCount.fetch_add(1, std::memory_order_relaxed);
                g_lastDropCaptureSlot.store(slot, std::memory_order_relaxed);
                g_lastDropCaptureVectorFlags.store(candidate.event.vectorFlags, std::memory_order_relaxed);
                r.pendingDropCandidates.erase(r.pendingDropCandidates.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }

        ReplayTick t{};
        t.pre = r.havePre ? r.pendingPre : post;
        t.post = post;
        t.weaponDefIndex = def;
        subtickCount = static_cast<uint32_t>(r.pendingSubs.size());
        t.numSubtick = subtickCount;
        t.eventFlags = r.pendingEventFlags;
        t.eventWeaponDefIndex = r.pendingDropEvent.weaponDefIndex;
        t.eventDropVectorFlags = r.pendingDropEvent.vectorFlags;
        t.eventDropTargetX = r.pendingDropEvent.target[0];
        t.eventDropTargetY = r.pendingDropEvent.target[1];
        t.eventDropTargetZ = r.pendingDropEvent.target[2];
        t.eventDropVelocityX = r.pendingDropEvent.velocity[0];
        t.eventDropVelocityY = r.pendingDropEvent.velocity[1];
        t.eventDropVelocityZ = r.pendingDropEvent.velocity[2];
        for (const auto& sm : r.pendingSubs)
            r.subs.push_back(sm);
        r.ticks.push_back(t);
        r.commands.push_back(r.havePendingCommand ? r.pendingCommand : ReplayCommandFrameData{});
        r.pendingSubs.clear();
        r.pendingCommand = {};
        r.havePendingCommand = false;
        r.havePre = false;
        r.pendingEventFlags = ReplayEventNone;
        r.pendingDropEvent = {};
        r.pendingDropEvent.weaponDefIndex = -1;
    }
}

int CopyTicks(int slot, ReplayTick* out, int maxTicks)
{
    if (!ValidSlot(slot) || !out || maxTicks <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    int n = static_cast<int>(r.ticks.size());
    n = std::min(n, maxTicks);
    for (int i = 0; i < n; ++i)
        out[i] = r.ticks[i];
    return n;
}

int CopySubticks(int slot, SubtickMove* out, int maxSubticks)
{
    if (!ValidSlot(slot) || !out || maxSubticks <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    int n = static_cast<int>(r.subs.size());
    n = std::min(n, maxSubticks);
    for (int i = 0; i < n; ++i)
        out[i] = r.subs[i];
    return n;
}

// Copies captured command frames into a caller-owned buffer
int CopyCommands(int slot, ReplayCommandFrameData* out, int maxCommands)
{
    if (!ValidSlot(slot) || !out || maxCommands <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    int n = static_cast<int>(r.commands.size());
    n = std::min(n, maxCommands);
    for (int i = 0; i < n; ++i)
        out[i] = r.commands[i];
    return n;
}

// ---- replay ----

// Load legacy buffers by supplying empty extended buffers
bool LoadReplay(int slot, const ReplayTick* ticks, int tickCount, const SubtickMove* subs, int subCount) noexcept
{
    return LoadReplayExtended(slot, ticks, tickCount, subs, subCount, nullptr, 0, nullptr, 0);
}

// Validate, stage, and atomically replace all replay buffers
bool LoadReplayExtended(int slot,
                        const ReplayTick* ticks,
                        int tickCount,
                        const SubtickMove* subs,
                        int subCount,
                        const ReplayCommandFrameData* commands,
                        int commandCount,
                        const ReplayMovementExtra* movementExtras,
                        int movementExtraCount) noexcept
{
    try
    {
        if (!ValidSlot(slot) || !ticks || tickCount < 0 || subCount < 0 || (subCount > 0 && !subs) ||
            (commandCount != 0 && commandCount != tickCount) || (commandCount > 0 && !commands) ||
            (movementExtraCount != 0 && movementExtraCount != tickCount) || (movementExtraCount > 0 && !movementExtras))
        {
            return false;
        }

        ReplayState& p = g_rep[slot];
        if (p.playing.load(std::memory_order_acquire)) return false;

        std::vector<ReplayTick> stagedTicks;
        std::vector<SubtickMove> stagedSubs;
        std::vector<ReplayCommandFrameData> stagedCommands;
        std::vector<ReplayMovementExtra> stagedMovementExtras;
        const auto subCountValue = static_cast<uint64_t>(subCount);
        std::vector<uint32_t> stagedOffsets(static_cast<size_t>(tickCount) + 1, 0);

        uint64_t totalSubticks = 0;
        for (int i = 0; i < tickCount; ++i)
        {
            if (ticks[i].numSubtick > kMaxSubtickPerTick) return false;
            stagedOffsets[static_cast<size_t>(i)] = static_cast<uint32_t>(totalSubticks);
            totalSubticks += ticks[i].numSubtick;
            if (totalSubticks > subCountValue) return false;
        }
        if (totalSubticks != subCountValue) return false;
        stagedOffsets[static_cast<size_t>(tickCount)] = static_cast<uint32_t>(totalSubticks);

        if (tickCount > 0) stagedTicks.assign(ticks, ticks + tickCount);
        if (subCount > 0) stagedSubs.assign(subs, subs + subCount);
        if (commandCount > 0) stagedCommands.assign(commands, commands + commandCount);
        if (movementExtraCount > 0)
        {
            stagedMovementExtras.assign(movementExtras, movementExtras + movementExtraCount);
        }

        std::scoped_lock lk(p.mu);
        if (p.playing.load(std::memory_order_acquire)) return false;

        p.ticks.swap(stagedTicks);
        p.subs.swap(stagedSubs);
        p.commands.swap(stagedCommands);
        p.movementExtras.swap(stagedMovementExtras);
        p.subOffset.swap(stagedOffsets);
        p.cursor.store(0, std::memory_order_relaxed);
        p.lastAppliedDef.store(-1, std::memory_order_relaxed);
        p.lastEventCursor = -1;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool StartReplay(int slot, bool loop)
{
    // Replay is an engine-control operation and is intentionally bot-only.
    // Recording may target humans, but recorded state must never be driven
    // back into a human slot.
    if (!bot_controller_hooks::IsLiveBotSlot(slot)) return false;

    ReplayState& p = g_rep[slot];
    {
        std::scoped_lock lk(p.mu);
        if (p.ticks.empty()) return false;
    }
    p.cursor.store(0, std::memory_order_relaxed);
    p.lastAppliedDef.store(-1, std::memory_order_relaxed);
    {
        std::scoped_lock lk(p.mu);
        p.lastEventCursor = -1;
    }
    p.loop.store(loop, std::memory_order_relaxed);
    p.playing.store(true, std::memory_order_release);
    input_injector::ClearUsercmdInjections(slot);
    return true;
}

bool StopReplay(int slot)
{
    if (!ValidSlot(slot)) return false;

    g_rep[slot].playing.store(false, std::memory_order_release);
    g_rep[slot].loop.store(false, std::memory_order_relaxed);
    input_injector::ClearReplayPawn(slot);

    return true;
}

bool IsReplaying(int slot)
{
    if (!ValidSlot(slot)) return false;

    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;

    // If the bot disappeared or the slot was reused, stop immediately instead
    // of allowing stale replay state to target another player.
    if (!bot_controller_hooks::IsLiveBotSlot(slot))
    {
        p.playing.store(false, std::memory_order_release);
        input_injector::ClearReplayPawn(slot);
        return false;
    }

    return true;
}

int ReplayCursor(int slot)
{
    if (!ValidSlot(slot)) return -1;
    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return -1;
    return p.cursor.load(std::memory_order_relaxed);
}

int ReplayTotal(int slot)
{
    if (!ValidSlot(slot)) return 0;
    ReplayState& p = g_rep[slot];
    std::scoped_lock lk(p.mu);
    return static_cast<int>(p.ticks.size());
}

// cursor points at the NEXT tick; the one just applied is cursor-1.
bool CurrentReplayTick(int slot, ReplayTick& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return false;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int idx = p.cursor.load(std::memory_order_relaxed) - 1;
    idx = std::max(idx, 0);
    if (idx >= total) return false;
    out = p.ticks[idx];
    return true;
}

// Assembles one complete PlayerRunCommand input frame from parallel replay buffers
bool ReplayCommandFrameForSimulation(int slot, ReplayCommandFrame& out)
{
    out = {};
    out.weaponSelect = -1;
    out.rawWeaponSelect = -1;
    if (!ValidSlot(slot)) return false;

    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return false;

    int recordedDef = -1;
    {
        std::scoped_lock lk(p.mu);
        const int total = static_cast<int>(p.ticks.size());
        const int cur = p.cursor.load(std::memory_order_relaxed);
        if (cur < 0 || cur >= total || p.subOffset.size() != p.ticks.size() + 1) return false;

        out.tick = p.ticks[static_cast<size_t>(cur)];
        recordedDef = out.tick.weaponDefIndex;
        out.commandView = out.tick.pre;
        out.buttons0 = out.tick.pre.buttons;
        out.buttons1 = out.tick.pre.buttons1;
        out.buttons2 = out.tick.pre.buttons2;

        const ReplayCommandFrameData* command =
            static_cast<size_t>(cur) < p.commands.size() ? &p.commands[static_cast<size_t>(cur)] : nullptr;
        const bool hasCommandButtons = command && (command->fields & kCommandFieldButtons) != 0;
        if (hasCommandButtons)
        {
            out.buttons0 = command->buttons;
            out.buttons1 = command->buttons1;
            out.buttons2 = command->buttons2;
        }
        else if (out.buttons1 == 0 && out.buttons2 == 0)
        {
            const uint64_t heldPrev = cur > 0 ? p.ticks[static_cast<size_t>(cur - 1)].pre.buttons : 0;
            out.buttons1 = out.buttons0 & ~heldPrev;
            out.buttons2 = heldPrev & ~out.buttons0;
        }

        if (command)
        {
            out.commandFields = command->fields;
            if ((command->fields & kCommandFieldViewAngles) != 0)
            {
                out.commandView.pitch = command->pitch;
                out.commandView.yaw = command->yaw;
                out.commandView.roll = command->roll;
            }
            if ((command->fields & kCommandFieldForwardMove) != 0) out.forwardMove = command->forwardMove;
            if ((command->fields & kCommandFieldLeftMove) != 0) out.leftMove = command->leftMove;
            if ((command->fields & kCommandFieldUpMove) != 0) out.upMove = command->upMove;
            if ((command->fields & kCommandFieldMouse) != 0)
            {
                out.mouseDx = command->mouseDx;
                out.mouseDy = command->mouseDy;
            }
            if ((command->fields & kCommandFieldWeaponSelect) != 0) out.rawWeaponSelect = command->weaponSelect;
            if ((command->fields & kCommandFieldLeftHand) != 0) out.leftHandDesired = command->leftHandDesired;
        }

        const uint32_t begin = p.subOffset[static_cast<size_t>(cur)];
        const uint32_t end = p.subOffset[static_cast<size_t>(cur) + 1];
        if (begin > end || end > p.subs.size()) return false;
        out.subtickCount = static_cast<int32_t>(end - begin);
        for (int i = 0; i < out.subtickCount; ++i)
            out.subticks[i] = p.subs[static_cast<size_t>(begin) + static_cast<size_t>(i)];
    }

    out.weaponSelect = ReplayWeaponSelectForDef(slot, recordedDef);
    return true;
}

bool ReplayCommandViewSnapshot(int slot, MovementSnapshot& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return false;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return false;
    out = p.ticks[cur].pre;
    return true;
}

int CurrentReplaySubticks(int slot, SubtickMove* out, int maxOut)
{
    if (!ValidSlot(slot) || !out || maxOut <= 0) return -1;
    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return -1;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int idx = p.cursor.load(std::memory_order_relaxed);
    if (idx < 0 || idx >= total) return -1;
    uint32_t begin = p.subOffset[idx];
    uint32_t end = p.subOffset[idx + 1];
    int n = static_cast<int>(end - begin);
    n = std::min(n, maxOut);
    for (int i = 0; i < n; ++i)
        out[i] = p.subs[begin + i];
    return n;
}

bool CurrentReplayInputButtons(int slot, uint64_t& b0, uint64_t& b1, uint64_t& b2)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return false;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return false;
    const MovementSnapshot& pre = p.ticks[cur].pre;
    b0 = pre.buttons;
    b1 = pre.buttons1;
    b2 = pre.buttons2;
    if (b1 == 0 && b2 == 0)
    {
        uint64_t heldPrev = (cur > 0) ? p.ticks[cur - 1].pre.buttons : 0;
        b1 = b0 & ~heldPrev;
        b2 = heldPrev & ~b0;
    }
    return true;
}

bool SwitchBotWeaponByDef(int slot, int defIndex)
{
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || defIndex < 0 || IsReplaying(slot)) return false;
    if (!weapon_locker_hooks::WeaponHooksReady()) return false;
    void* ws = weapon_locker_hooks::WsForSlot(slot);
    if (!ws) return false;
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, defIndex);
    if (!weapon) return false;
    return weapon_locker_hooks::SelectWeaponRaw(ws, weapon);
}

// Def index of the bot's current active weapon
int BotActiveWeaponDef(int slot)
{
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || !weapon_locker_hooks::WeaponHooksReady()) return -1;
    void* ws = weapon_locker_hooks::WsForSlot(slot);
    if (!ws) return -1;
    return weapon_locker_hooks::ActiveWeaponDef(ws);
}

// Treats CT and T fire grenades as the same replay weapon type
bool ReplayWeaponDefsMatch(int firstDef, int secondDef)
{
    return firstDef == secondDef || (IsFireGrenadeDef(firstDef) && IsFireGrenadeDef(secondDef));
}

// Entity index for cmd.weaponselect this replay tick
int CurrentReplayWeaponDef(int slot)
{
    if (!ValidSlot(slot)) return -1;
    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return -1;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return -1;
    return p.ticks[cur].weaponDefIndex;
}

// Resolves and applies the recorded weapon definition for one command frame
namespace {

int ReplayWeaponSelectForDef(int slot, int recordedDef)
{
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || !weapon_locker_hooks::WeaponHooksReady()) return -1;
    if (recordedDef < 0) return -1;

    void* ws = weapon_locker_hooks::WsForSlot(slot);
    if (!ws) return -1;

    // Already holding the recorded weapon -> no switch
    if (ReplayWeaponDefsMatch(weapon_locker_hooks::ActiveWeaponDef(ws), recordedDef))
    {
        g_rep[slot].lastAppliedDef.store(recordedDef, std::memory_order_relaxed);
        return -1;
    }

    void* weapon = FindReplayWeaponByDef(ws, recordedDef);
    if (!weapon) return -1;
    weapon_locker_hooks::SelectWeaponRaw(ws, weapon);
    g_rep[slot].lastAppliedDef.store(recordedDef, std::memory_order_relaxed);
    return weapon_locker_hooks::WeaponEntIndex(weapon);
}

// Resolves the current tick's recorded weapon to a live entity index
int CurrentReplayWeaponSelect(int slot) { return ReplayWeaponSelectForDef(slot, CurrentReplayWeaponDef(slot)); }

// Returns the current drop event only once for each replay cursor
} // namespace

bool TakeCurrentReplayDrop(int slot, ReplayDropEvent& event)
{
    event = {};
    event.weaponDefIndex = -1;
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!IsReplaying(slot)) return false;

    std::scoped_lock lk(p.mu);
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || static_cast<size_t>(cur) >= p.ticks.size() || p.lastEventCursor == cur) return false;
    p.lastEventCursor = cur;

    const ReplayTick& tick = p.ticks[cur];
    if ((tick.eventFlags & ReplayEventDrop) == 0) return false;
    event.weaponDefIndex = tick.eventWeaponDefIndex;
    event.vectorFlags = tick.eventDropVectorFlags;
    event.target[0] = tick.eventDropTargetX;
    event.target[1] = tick.eventDropTargetY;
    event.target[2] = tick.eventDropTargetZ;
    event.velocity[0] = tick.eventDropVelocityX;
    event.velocity[1] = tick.eventDropVelocityY;
    event.velocity[2] = tick.eventDropVelocityZ;
    return true;
}

// Dispatches the same client command path used when a player presses G
bool DropReplayEventWeapon(int slot, void* services, const ReplayDropEvent& event)
{
    const int weaponDefIndex = event.weaponDefIndex;
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || !services || weaponDefIndex < 0 || !IsReplaying(slot) ||
        !weapon_locker_hooks::WeaponHooksReady())
        return false;

    g_dropReplayAttemptCount.fetch_add(1, std::memory_order_relaxed);
    g_lastDropReplaySlot.store(slot, std::memory_order_relaxed);
    g_lastDropReplayWeaponDef.store(weaponDefIndex, std::memory_order_relaxed);
    g_lastDropReplayVectorFlags.store(event.vectorFlags, std::memory_order_relaxed);

    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    void* ws = nullptr;
    if (!pawn || !GuardedRead(pawn, tg::g_pawnWeaponServices, ws) || !ws) return false;
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, weaponDefIndex);
    if (!weapon) return false;
    if (weapon_locker_hooks::ActiveWeaponDef(ws) != weaponDefIndex && !weapon_locker_hooks::SelectWeaponRaw(ws, weapon)) return false;
    if (weapon_locker_hooks::ActiveWeaponDef(ws) != weaponDefIndex) return false;

    if (!dispatch::g_gameClients) return false;
    CCommand command;
    if (!command.Tokenize("drop")) return false;

    g_activeReplayDropSlot = slot;
    g_activeReplayDropEvent = &event;
    dispatch::g_gameClients->ClientCommand(CPlayerSlot(slot), command);
    g_activeReplayDropEvent = nullptr;
    g_activeReplayDropSlot = -1;
    g_dropReplayNativeCallCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Returns how many player drop commands were recorded
uint64_t DropCaptureCount() { return g_dropCaptureCount.load(std::memory_order_relaxed); }

// Returns how many real weapon-service drops reached the native hook
uint64_t DropHookCallCount() { return g_dropHookCallCount.load(std::memory_order_relaxed); }

// Returns how many native drop calls belonged to an active recording slot
uint64_t DropHookRecordingCallCount() { return g_dropHookRecordingCallCount.load(std::memory_order_relaxed); }

// Returns how many recording calls actually detached their supplied weapon
uint64_t DropHookPhysicalDropCount() { return g_dropHookPhysicalDropCount.load(std::memory_order_relaxed); }

// Returns how many recording drop calls had no resolvable item definition
uint64_t DropHookInvalidDefCount() { return g_dropHookInvalidDefCount.load(std::memory_order_relaxed); }

// Returns how many recorded drop events reached replay execution
uint64_t DropReplayAttemptCount() { return g_dropReplayAttemptCount.load(std::memory_order_relaxed); }

// Returns how many replay drop calls re-entered the confirmed native hook
uint64_t DropReplayHookCallCount() { return g_dropReplayHookCallCount.load(std::memory_order_relaxed); }

// Returns how many replay calls applied at least one recorded drop vector
uint64_t DropReplayVectorOverrideCount() { return g_dropReplayVectorOverrideCount.load(std::memory_order_relaxed); }

// Returns how many replay calls actually detached their supplied weapon
uint64_t DropReplayDetachedCount() { return g_dropReplayDetachedCount.load(std::memory_order_relaxed); }

// Returns how many native drop calls were issued
uint64_t DropReplayNativeCallCount() { return g_dropReplayNativeCallCount.load(std::memory_order_relaxed); }

// Reports whether the native weapon-service drop hook is installed
bool DropHookReady() { return g_dropHookReady.load(std::memory_order_acquire); }

// Returns the address resolved from the live weapon-services vtable
void* DropHookAddress() { return g_addrDropWeapon; }

// Returns the last player slot whose drop command was captured
int LastDropCaptureSlot() { return g_lastDropCaptureSlot.load(std::memory_order_relaxed); }

// Returns which drop vectors were saved for the latest captured event
uint32_t LastDropCaptureVectorFlags() { return g_lastDropCaptureVectorFlags.load(std::memory_order_relaxed); }

// Returns the recording slot resolved for the latest native drop call
int LastDropHookSlot() { return g_lastDropHookSlot.load(std::memory_order_relaxed); }

// Returns the item definition read from the latest native drop call
int LastDropHookWeaponDef() { return g_lastDropHookWeaponDef.load(std::memory_order_relaxed); }

// Reports whether the latest native drop call belonged to an active recorder
bool LastDropHookWasRecording() { return g_lastDropHookWasRecording.load(std::memory_order_relaxed); }

// Returns the pawn read from the latest native drop caller
void* LastDropHookPawn() { return g_lastDropHookPawn.load(std::memory_order_relaxed); }

// Returns the optional target pointer from the latest native drop call
void* LastDropHookTarget() { return g_lastDropHookTarget.load(std::memory_order_relaxed); }

// Returns the optional velocity pointer from the latest native drop call
void* LastDropHookVelocity() { return g_lastDropHookVelocity.load(std::memory_order_relaxed); }

// Returns the last bot slot that attempted a replay drop
int LastDropReplaySlot() { return g_lastDropReplaySlot.load(std::memory_order_relaxed); }

// Returns the last recorded item definition used by replay drop
int LastDropReplayWeaponDef() { return g_lastDropReplayWeaponDef.load(std::memory_order_relaxed); }

// Returns which recorded drop vectors were supplied to the latest replay call
uint32_t LastDropReplayVectorFlags() { return g_lastDropReplayVectorFlags.load(std::memory_order_relaxed); }

// Write replay velocity onto the pawn. View replay is driven by SetEyeAngles.
namespace {

void WriteVelocityToPawn(int slot, void* services, const MovementSnapshot& s)
{
    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn) return;
    WriteVector3(pawn, tg::g_entAbsVelocity, s.velX, s.velY, s.velZ);
}

// Writes replay origin through the current body-component scene node.
void WriteSceneNodeOrigin(int slot, void* services, const MovementSnapshot& s, float zBias = 0.0F)
{
    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn) return;

    void* node = ResolveSceneNode(pawn);
    if (!node) return;

    const float values[3] = { s.originX, s.originY, s.originZ + zBias };
    TryWriteMemoryGuarded(node, tg::g_nodeAbsOrigin, values, sizeof(values));
}

// Write origin + velocity into CMoveData.
void WriteMoveData(void* moveData, const MovementSnapshot& s)
{
    WriteVector3(moveData, tg::g_moveAbsOrigin, s.originX, s.originY, s.originZ);
    WriteVector3(moveData, tg::g_moveVelocity, s.velX, s.velY, s.velZ);
}

// Restores duck and ladder state through guarded field writes.
void WriteMovementServiceState(void* services, const MovementSnapshot& s)
{
    WriteField(services, tg::g_servicesDuckAmount, s.duckAmount);
    WriteField(services, tg::g_servicesDuckSpeed, s.duckSpeed);
    WriteVector3(services, tg::g_servicesLadderNormal, s.ladderNormalX, s.ladderNormalY, s.ladderNormalZ);
    WriteField(services, tg::g_servicesDucked, s.ducked);
    WriteField(services, tg::g_servicesDucking, s.ducking);
    WriteField(services, tg::g_servicesDesiresDuck, s.desiresDuck);
}

// Normalizes replay yaw to the engine's signed degree range
float NormalizeReplayYaw(float yaw)
{
    yaw = std::fmod(yaw + 180.0F, 360.0F);
    if (yaw < 0.0F) yaw += 360.0F;
    return yaw - 180.0F;
}

// Writes command view angles to the pawn fields read before movement processing
void WriteRawViewAnglesToPawn(void* pawn, float pitch, float yaw)
{
    const float normalizedYaw = NormalizeReplayYaw(yaw);
    WriteVector3(pawn, tg::g_pawnViewAngle, pitch, normalizedYaw, 0.0F);
    WriteVector3(pawn, tg::g_pawnEyeAngles, pitch, normalizedYaw, 0.0F);
}

// Keeps the pawn and movement-service view history aligned with the command
void WriteReplayViewHistory(void* services, void* pawn, float pitch, float yaw)
{
    const float normalizedYaw = NormalizeReplayYaw(yaw);
    WriteVector3(pawn, tg::g_pawnViewAnglePrevious, pitch, normalizedYaw, 0.0F);
    WriteVector3(services, tg::g_servicesOldViewAngles, pitch, normalizedYaw, 0.0F);
}

// Seeds pawn state before weapon and grenade code consumes the replayed command
} // namespace

void OnReplayCommandPre(int slot, void* services, const ReplayTick& tick, const MovementSnapshot& commandView)
{
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || !services || !IsReplaying(slot)) return;

    WriteVelocityToPawn(slot, services, tick.pre);
    WriteMovementServiceState(services, tick.pre);

    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn) return;

    WriteField(pawn, tg::g_entMoveType, tick.pre.moveType);
    WriteField(pawn, tg::g_entActualMoveType, tick.pre.actualMoveType);
    WriteSceneNodeOrigin(slot, services, tick.pre);
    bot_controller_hooks::ApplyReplayEyeAngles(pawn, commandView.pitch, commandView.yaw);
    WriteRawViewAnglesToPawn(pawn, commandView.pitch, commandView.yaw);
    WriteReplayViewHistory(services, pawn, commandView.pitch, commandView.yaw);
}

// ProcessMovement (pre): seed CMoveData + pawn + moveType with pre state.
void OnReplayPre(int slot, void* services, void* moveData)
{
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || !services || !moveData || !IsReplaying(slot)) return;
    ReplayState& p = g_rep[slot];
    ReplayTick t{};
    {
        std::scoped_lock lk(p.mu);
        int total = static_cast<int>(p.ticks.size());
        int cur = p.cursor.load(std::memory_order_relaxed);
        if (cur >= total) return; // commit handler will stop/loop
        t = p.ticks[cur];
    }
    WriteMoveData(moveData, t.pre);
    WriteVelocityToPawn(slot, services, t.pre);
    WriteMovementServiceState(services, t.pre);
    // Feed recorded buttons so the engine's Duck()/ladder logic runs
    WriteField(services, tg::g_servicesButtons, t.pre.buttons);
    WriteField(services, tg::g_servicesButtons1, t.pre.buttons1);
    WriteField(services, tg::g_servicesButtons2, t.pre.buttons2);
    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (pawn)
    {
        WriteField(pawn, tg::g_entMoveType, t.pre.moveType);
        WriteSceneNodeOrigin(slot, services, t.pre);
        bot_controller_hooks::ApplyReplayEyeAngles(pawn, t.pre.pitch, t.pre.yaw);
    }
}

// FinishMove (pre): write post snapshot into CMoveData + scene-node origin.
void OnReplayFinishMove(int slot, void* services, void* moveData)
{
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || !services || !moveData || !IsReplaying(slot)) return;
    ReplayState& p = g_rep[slot];
    ReplayTick t{};
    {
        std::scoped_lock lk(p.mu);
        int total = static_cast<int>(p.ticks.size());
        int cur = p.cursor.load(std::memory_order_relaxed);
        if (cur >= total) return;
        t = p.ticks[cur];
    }
    WriteMoveData(moveData, t.post);
    WriteSceneNodeOrigin(slot, services, t.post, 1000.0F);
}

void OnReplayCommit(int slot, void* services)
{
    if (!bot_controller_hooks::IsLiveBotSlot(slot) || !services || !IsReplaying(slot)) return;
    ReplayState& p = g_rep[slot];

    ReplayTick t{};
    int cur;
    int total;
    {
        std::scoped_lock lk(p.mu);
        total = static_cast<int>(p.ticks.size());
        cur = p.cursor.load(std::memory_order_relaxed);
        if (cur >= total)
        {
            if (p.loop.load(std::memory_order_relaxed) && total > 0)
            {
                p.cursor.store(0, std::memory_order_relaxed);
                p.lastAppliedDef.store(-1, std::memory_order_relaxed);
                p.lastEventCursor = -1;
                return;
            }
            p.playing.store(false, std::memory_order_release);
            input_injector::ClearReplayPawn(slot);
            return;
        }
        t = p.ticks[cur];
    }

    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (pawn)
    {
        WriteField(pawn, tg::g_entMoveType, t.post.moveType);
        WriteField(pawn, tg::g_entActualMoveType, t.post.actualMoveType);
        // Merge ground + ducking bits from the recording, keep the rest live.
        uint32_t live = 0;
        uint32_t mask = tg::kFlOnGround | tg::kFlDucking;
        if (SafeRead(pawn, tg::g_entFlags, live))
        {
            live = (live & ~mask) | (t.post.entityFlags & mask);
            WriteField(pawn, tg::g_entFlags, live);
        }
        bot_controller_hooks::ApplyReplayEyeAngles(pawn, t.post.pitch, t.post.yaw);
    }

    WriteVelocityToPawn(slot, services, t.post);
    WriteSceneNodeOrigin(slot, services, t.post);
    WriteMovementServiceState(services, t.post);

    p.cursor.store(cur + 1, std::memory_order_relaxed);
}

void ClearAll()
{
    // Remove the lazy global DropWeapon hook first. NativeHook::Remove()
    // drains active callbacks before returning, so no new drop frame can race
    // with the state reset below.
    g_dropHookReady.store(false, std::memory_order_release);
    g_hookDropWeapon.Remove();
    g_addrDropWeapon = nullptr;
    g_dropHookTried.store(false, std::memory_order_release);

    // Clear thread-local replay/drop context for the calling game thread.
    g_activeReplayDropSlot = -1;
    g_activeReplayDropEvent = nullptr;
    g_dropFrames.clear();

    for (int i = 0; i < kMaxSlots; ++i)
    {
        g_rec[i].recording.store(false, std::memory_order_release);
        g_rep[i].playing.store(false, std::memory_order_release);
        g_rep[i].loop.store(false, std::memory_order_relaxed);

        {
            std::scoped_lock lk(g_rec[i].mu);
            g_rec[i].ticks.clear();
            g_rec[i].subs.clear();
            g_rec[i].commands.clear();
            g_rec[i].pendingSubs.clear();
            g_rec[i].pendingCommand = {};
            g_rec[i].havePendingCommand = false;
            g_rec[i].pendingPre = {};
            g_rec[i].havePre = false;
            g_rec[i].pendingEventFlags = ReplayEventNone;
            g_rec[i].pendingDropEvent = {};
            g_rec[i].pendingDropEvent.weaponDefIndex = -1;
            g_rec[i].pendingDropCandidates.clear();
        }

        {
            std::scoped_lock lk(g_rep[i].mu);
            g_rep[i].ticks.clear();
            g_rep[i].subs.clear();
            g_rep[i].commands.clear();
            g_rep[i].movementExtras.clear();
            g_rep[i].subOffset.clear();
            g_rep[i].lastEventCursor = -1;
        }

        g_rec[i].currentDef.store(-1, std::memory_order_relaxed);
        g_rec[i].liveWs.store(nullptr, std::memory_order_relaxed);
        g_rep[i].cursor.store(0, std::memory_order_relaxed);
        g_rep[i].lastAppliedDef.store(-1, std::memory_order_relaxed);

        input_injector::ClearReplayPawn(i);
    }
}
} // namespace motion_recorder
} // namespace cs2bc
