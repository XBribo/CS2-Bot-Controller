// Motion recording & replay implementation

#include "MotionRecorder.h"
#include "BotController.h"
#include "dispatch.h"
#include "InputInjector.h"
#include "WeaponLocker.h"
#include "ccsbot_slot.h"
#include "hook.h"
#include "version_targets.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

namespace tg = BotController::targets;

namespace BotController {
namespace MotionRecorder {
#if defined(_WIN32)
using DropWeaponResult = uint8_t;
#else
using DropWeaponResult = void*;
#endif
using DropWeapon_t = DropWeaponResult(BC_FASTCALL*)(void* weaponServices, void* weapon, void* target, void* velocity);

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
    // Subtick moves seen on PlayerRunCommand, awaiting the matching
    // ProcessMovement post that commits them to a tick.
    std::vector<SubtickMove> pendingSubs;
    MovementSnapshot pendingPre{};
    bool havePre{ false };
    uint32_t pendingEventFlags{ ReplayEvent_None };
    ReplayDropEvent pendingDropEvent{ -1, ReplayDropVector_None, {}, {} };
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

static std::array<RecordState, kMaxSlots> g_rec;
static std::array<ReplayState, kMaxSlots> g_rep;
static std::atomic<uint64_t> g_dropHookCallCount{ 0 };
static std::atomic<uint64_t> g_dropHookRecordingCallCount{ 0 };
static std::atomic<uint64_t> g_dropHookPhysicalDropCount{ 0 };
static std::atomic<uint64_t> g_dropHookInvalidDefCount{ 0 };
static std::atomic<uint64_t> g_dropCaptureCount{ 0 };
static std::atomic<uint64_t> g_dropReplayAttemptCount{ 0 };
static std::atomic<uint64_t> g_dropReplayHookCallCount{ 0 };
static std::atomic<uint64_t> g_dropReplayVectorOverrideCount{ 0 };
static std::atomic<uint64_t> g_dropReplayDetachedCount{ 0 };
static std::atomic<uint64_t> g_dropReplayNativeCallCount{ 0 };
static std::atomic<int> g_lastDropCaptureSlot{ -1 };
static std::atomic<uint32_t> g_lastDropCaptureVectorFlags{ ReplayDropVector_None };
static std::atomic<int> g_lastDropHookSlot{ -1 };
static std::atomic<int> g_lastDropHookWeaponDef{ -1 };
static std::atomic<bool> g_lastDropHookWasRecording{ false };
static std::atomic<void*> g_lastDropHookPawn{ nullptr };
static std::atomic<void*> g_lastDropHookTarget{ nullptr };
static std::atomic<void*> g_lastDropHookVelocity{ nullptr };
static std::atomic<int> g_lastDropReplaySlot{ -1 };
static std::atomic<int> g_lastDropReplayWeaponDef{ -1 };
static std::atomic<uint32_t> g_lastDropReplayVectorFlags{ ReplayDropVector_None };
static Hook g_hookDropWeapon;
static DropWeapon_t g_origDropWeapon = nullptr;
static void* g_addrDropWeapon = nullptr;
static std::atomic<bool> g_dropHookTried{ false };
static std::atomic<bool> g_dropHookReady{ false };
static thread_local int g_activeReplayDropSlot = -1;
static thread_local const ReplayDropEvent* g_activeReplayDropEvent = nullptr;
constexpr int kMolotovDef = 46;
constexpr int kIncendiaryDef = 48;

static bool ValidSlot(int s) { return s >= 0 && s < kMaxSlots; }

// Returns whether an item definition is either faction's fire grenade
static bool IsFireGrenadeDef(int defIndex)
{
    return defIndex == kMolotovDef || defIndex == kIncendiaryDef;
}

// Prefers the recorded fire grenade and falls back to the other faction's variant
static void* FindReplayWeaponByDef(void* ws, int recordedDef)
{
    void* weapon = WeaponLockerHooks::FindWeaponByDef(ws, recordedDef);
    if (weapon || !IsFireGrenadeDef(recordedDef)) return weapon;

    const int alternateDef = recordedDef == kMolotovDef ? kIncendiaryDef : kMolotovDef;
    return WeaponLockerHooks::FindWeaponByDef(ws, alternateDef);
}

// Copies an optional engine Vector into stable recording storage
static bool ReadDropVector(void* vector, float out[3])
{
    return vector && TryReadMemory(vector, 0, out, sizeof(float) * 3);
}

// Prefers the recorder's exact cached weapon-services owner over controller handles
static int RecordingSlotForWeaponServices(void* weaponServices, void* pawn)
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        RecordState& r = g_rec[slot];
        if (r.recording.load(std::memory_order_acquire) &&
            r.liveWs.load(std::memory_order_relaxed) == weaponServices)
            return slot;
    }

    const int slot = ControllerSlotForPawn(pawn);
    return IsRecording(slot) ? slot : -1;
}

// Resolves the replay slot that currently owns this weapon-services pointer
static int ReplaySlotForWeaponServices(void* weaponServices)
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        if (IsReplaying(slot) && WeaponLockerHooks::WsForSlot(slot) == weaponServices) return slot;
    }
    return -1;
}

// Stores one real weapon-service drop in the pending recording tick
static bool CaptureDropEvent(int slot, const ReplayDropEvent& event)
{
    if (!ValidSlot(slot) || event.weaponDefIndex < 0) return false;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return false;

    std::lock_guard<std::mutex> lk(r.mu);
    r.pendingEventFlags |= ReplayEvent_Drop;
    r.pendingDropEvent = event;
    g_dropCaptureCount.fetch_add(1, std::memory_order_relaxed);
    g_lastDropCaptureSlot.store(slot, std::memory_order_relaxed);
    g_lastDropCaptureVectorFlags.store(event.vectorFlags, std::memory_order_relaxed);
    return true;
}

// Captures only calls that actually detach the supplied weapon from its owner
static DropWeaponResult BC_FASTCALL HookedDropWeapon(void* weaponServices, void* weapon, void* target, void* velocity)
{
    g_dropHookCallCount.fetch_add(1, std::memory_order_relaxed);

    void* pawn = nullptr;
    if (weaponServices) GuardedRead(weaponServices, tg::kServices_Pawn, pawn);
    const int recordingSlot = RecordingSlotForWeaponServices(weaponServices, pawn);
    const int replaySlot = ValidSlot(g_activeReplayDropSlot) ? g_activeReplayDropSlot : ReplaySlotForWeaponServices(weaponServices);
    const int slot = ValidSlot(recordingSlot) ? recordingSlot : replaySlot;
    int weaponDefIndex = WeaponLockerHooks::ReadDefIndex(weapon);
    if (weaponDefIndex < 0 && weaponServices) weaponDefIndex = WeaponLockerHooks::ActiveWeaponDef(weaponServices);
    if (weaponDefIndex < 0 && ValidSlot(recordingSlot))
        weaponDefIndex = g_rec[recordingSlot].currentDef.load(std::memory_order_relaxed);

    ReplayDropEvent recordedEvent{};
    recordedEvent.weaponDefIndex = weaponDefIndex;
    if (ReadDropVector(target, recordedEvent.target)) recordedEvent.vectorFlags |= ReplayDropVector_Target;
    if (ReadDropVector(velocity, recordedEvent.velocity)) recordedEvent.vectorFlags |= ReplayDropVector_Velocity;

    float replayTarget[3] = {};
    float replayVelocity[3] = {};
    void* effectiveTarget = target;
    void* effectiveVelocity = velocity;
    if (g_activeReplayDropEvent)
    {
        if (g_activeReplayDropEvent->vectorFlags != ReplayDropVector_None)
            g_dropReplayVectorOverrideCount.fetch_add(1, std::memory_order_relaxed);
        if ((g_activeReplayDropEvent->vectorFlags & ReplayDropVector_Target) != 0)
        {
            for (int i = 0; i < 3; ++i) replayTarget[i] = g_activeReplayDropEvent->target[i];
            effectiveTarget = replayTarget;
        }
        if ((g_activeReplayDropEvent->vectorFlags & ReplayDropVector_Velocity) != 0)
        {
            for (int i = 0; i < 3; ++i) replayVelocity[i] = g_activeReplayDropEvent->velocity[i];
            effectiveVelocity = replayVelocity;
        }
    }

    if (ValidSlot(recordingSlot))
    {
        g_dropHookRecordingCallCount.fetch_add(1, std::memory_order_relaxed);
        if (weaponDefIndex < 0) g_dropHookInvalidDefCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (ValidSlot(g_activeReplayDropSlot)) g_dropReplayHookCallCount.fetch_add(1, std::memory_order_relaxed);

    g_lastDropHookPawn.store(pawn, std::memory_order_relaxed);
    g_lastDropHookSlot.store(slot, std::memory_order_relaxed);
    g_lastDropHookWeaponDef.store(weaponDefIndex, std::memory_order_relaxed);
    g_lastDropHookWasRecording.store(ValidSlot(recordingSlot), std::memory_order_relaxed);
    g_lastDropHookTarget.store(effectiveTarget, std::memory_order_relaxed);
    g_lastDropHookVelocity.store(effectiveVelocity, std::memory_order_relaxed);

    const DropWeaponResult result = g_origDropWeapon(weaponServices, weapon, effectiveTarget, effectiveVelocity);
    const bool detached = weaponServices && weapon && weaponDefIndex >= 0 &&
                          WeaponLockerHooks::FindWeaponByDef(weaponServices, weaponDefIndex) != weapon;
    if (detached && ValidSlot(recordingSlot))
    {
        g_dropHookPhysicalDropCount.fetch_add(1, std::memory_order_relaxed);
        CaptureDropEvent(recordingSlot, recordedEvent);
    }
    else if (ValidSlot(recordingSlot) && weapon && weaponDefIndex >= 0)
    {
        RecordState& r = g_rec[recordingSlot];
        std::lock_guard<std::mutex> lk(r.mu);
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
        if (!found) r.pendingDropCandidates.push_back({ weapon, recordedEvent });
    }
    if (detached && ValidSlot(g_activeReplayDropSlot)) g_dropReplayDetachedCount.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// Installs the drop hook from a live weapon-services vtable once
static void EnsureDropWeaponHook(void* weaponServices)
{
    if (!weaponServices || g_dropHookTried.exchange(true, std::memory_order_acq_rel)) return;

    void** vtable = nullptr;
    if (!GuardedRead(weaponServices, 0, vtable) || !vtable) return;
    if (!GuardedRead(vtable, tg::kVtIdx_DropWeapon * static_cast<int>(sizeof(void*)), g_addrDropWeapon) || !g_addrDropWeapon)
        return;

    if (g_hookDropWeapon.Create(g_addrDropWeapon, reinterpret_cast<void*>(&HookedDropWeapon),
                                reinterpret_cast<void**>(&g_origDropWeapon)) &&
        g_hookDropWeapon.Enable())
    {
        g_dropHookReady.store(true, std::memory_order_release);
        return;
    }

    g_hookDropWeapon.Remove();
    g_origDropWeapon = nullptr;
    g_addrDropWeapon = nullptr;
}

// Reads a three-float engine vector through one guarded memory operation.
static bool ReadVector3(void* base, int offset, float& x, float& y, float& z)
{
    float values[3] = {};
    if (!TryReadMemory(base, offset, values, sizeof(values))) return false;
    x = values[0];
    y = values[1];
    z = values[2];
    return true;
}

// Writes a three-float engine vector through one guarded memory operation.
static bool WriteVector3(void* base, int offset, float x, float y, float z)
{
    const float values[3] = { x, y, z };
    return TryWriteMemory(base, offset, values, sizeof(values));
}

// Resolves the current scene node through the July 2026 body component layout.
static void* ResolveSceneNode(void* entity)
{
    void* body = nullptr;
    if (!GuardedRead(entity, tg::kEnt_BodyComponent, body) || !body) return nullptr;

    void* node = nullptr;
    return GuardedRead(body, tg::kBody_SceneNode, node) ? node : nullptr;
}

// Read a MovementSnapshot from live engine state (services -> pawn).
static bool ReadSnapshot(int slot, void* services, MovementSnapshot& out)
{
    if (!services) return false;
    void* pawn = InputInjector::ResolveReplayPawn(slot, services);
    if (!pawn) return false;

    void* node = ResolveSceneNode(pawn);
    return node && ReadVector3(pawn, tg::kEnt_AbsVelocity, out.velX, out.velY, out.velZ) &&
           SafeRead(pawn, tg::kEnt_Flags, out.entityFlags) && SafeRead(pawn, tg::kEnt_MoveType, out.moveType) &&
           SafeRead(pawn, tg::kEnt_ActualMoveType, out.actualMoveType) && SafeRead(services, tg::kServices_Buttons, out.buttons) &&
           SafeRead(services, tg::kServices_Buttons1, out.buttons1) && SafeRead(services, tg::kServices_Buttons2, out.buttons2) &&
           SafeRead(services, tg::kServices_DuckAmount, out.duckAmount) && SafeRead(services, tg::kServices_DuckSpeed, out.duckSpeed) &&
           ReadVector3(services, tg::kServices_LadderNormal, out.ladderNormalX, out.ladderNormalY, out.ladderNormalZ) &&
           SafeRead(services, tg::kServices_Ducked, out.ducked) && SafeRead(services, tg::kServices_Ducking, out.ducking) &&
           SafeRead(services, tg::kServices_DesiresDuck, out.desiresDuck) &&
           ReadVector3(pawn, tg::kPawn_ViewAngle, out.pitch, out.yaw, out.roll) &&
           ReadVector3(node, tg::kNode_AbsOrigin, out.originX, out.originY, out.originZ);
}

// ---- recording ----

bool StartRecord(int slot)
{
    if (!ValidSlot(slot)) return false;
    RecordState& r = g_rec[slot];
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.ticks.clear();
        r.subs.clear();
        r.pendingSubs.clear();
        r.havePre = false;
        r.pendingEventFlags = ReplayEvent_None;
        r.pendingDropEvent = {};
        r.pendingDropEvent.weaponDefIndex = -1;
        r.pendingDropCandidates.clear();
        r.ticks.reserve(4096); // ~64s @ 64 tick
        r.subs.reserve(4096);
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
    std::lock_guard<std::mutex> lk(r.mu);
    return static_cast<int>(r.ticks.size());
}

int RecordedSubtickCount(int slot)
{
    if (!ValidSlot(slot)) return -1;
    RecordState& r = g_rec[slot];
    std::lock_guard<std::mutex> lk(r.mu);
    return static_cast<int>(r.subs.size());
}

void SetLiveWs(int slot, void* ws)
{
    if (!ValidSlot(slot)) return;
    g_rec[slot].liveWs.store(ws, std::memory_order_relaxed);
    EnsureDropWeaponHook(ws);
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
    std::lock_guard<std::mutex> lk(r.mu);
    r.pendingPre = pre;
    r.havePre = true;
}

void OnCaptureSubticks(int slot, const SubtickMove* moves, int count)
{
    if (!ValidSlot(slot) || count < 0) return;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return;
    if (count > kMaxSubtickPerTick) count = kMaxSubtickPerTick;
    std::lock_guard<std::mutex> lk(r.mu);
    r.pendingSubs.clear();
    for (int i = 0; i < count; ++i)
        r.pendingSubs.push_back(moves[i]);
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
        ReadVector3(cmd, tg::kMove_AbsOrigin, post.originX, post.originY, post.originZ);
    }

    // Active weapon def for this tick.
    void* ws = r.liveWs.load(std::memory_order_relaxed);
    int def = WeaponLockerHooks::ActiveWeaponDef(ws);
    if (def < 0) def = r.currentDef.load(std::memory_order_relaxed);
    if (def >= 0) r.currentDef.store(def, std::memory_order_relaxed);

    uint32_t nSub;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        for (size_t i = 0; i < r.pendingDropCandidates.size();)
        {
            const RecordState::DropCandidate& candidate = r.pendingDropCandidates[i];
            if (WeaponLockerHooks::FindWeaponByDef(ws, candidate.event.weaponDefIndex) != candidate.weapon)
            {
                r.pendingEventFlags |= ReplayEvent_Drop;
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
        nSub = static_cast<uint32_t>(r.pendingSubs.size());
        t.numSubtick = nSub;
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
        r.pendingSubs.clear();
        r.havePre = false;
        r.pendingEventFlags = ReplayEvent_None;
        r.pendingDropEvent = {};
        r.pendingDropEvent.weaponDefIndex = -1;
    }
}

int CopyTicks(int slot, ReplayTick* out, int maxTicks)
{
    if (!ValidSlot(slot) || !out || maxTicks <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::lock_guard<std::mutex> lk(r.mu);
    int n = static_cast<int>(r.ticks.size());
    if (n > maxTicks) n = maxTicks;
    for (int i = 0; i < n; ++i)
        out[i] = r.ticks[i];
    return n;
}

int CopySubticks(int slot, SubtickMove* out, int maxSubticks)
{
    if (!ValidSlot(slot) || !out || maxSubticks <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::lock_guard<std::mutex> lk(r.mu);
    int n = static_cast<int>(r.subs.size());
    if (n > maxSubticks) n = maxSubticks;
    for (int i = 0; i < n; ++i)
        out[i] = r.subs[i];
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
        std::vector<uint32_t> stagedOffsets(static_cast<size_t>(tickCount) + 1, 0);

        uint64_t totalSubticks = 0;
        for (int i = 0; i < tickCount; ++i)
        {
            if (ticks[i].numSubtick > kMaxSubtickPerTick) return false;
            stagedOffsets[static_cast<size_t>(i)] = static_cast<uint32_t>(totalSubticks);
            totalSubticks += ticks[i].numSubtick;
            if (totalSubticks > static_cast<uint64_t>(subCount)) return false;
        }
        if (totalSubticks != static_cast<uint64_t>(subCount)) return false;
        stagedOffsets[static_cast<size_t>(tickCount)] = static_cast<uint32_t>(totalSubticks);

        if (tickCount > 0) stagedTicks.assign(ticks, ticks + tickCount);
        if (subCount > 0) stagedSubs.assign(subs, subs + subCount);
        if (commandCount > 0) stagedCommands.assign(commands, commands + commandCount);
        if (movementExtraCount > 0)
        {
            stagedMovementExtras.assign(movementExtras, movementExtras + movementExtraCount);
        }

        std::lock_guard<std::mutex> lk(p.mu);
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
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    {
        std::lock_guard<std::mutex> lk(p.mu);
        if (p.ticks.empty()) return false;
    }
    p.cursor.store(0, std::memory_order_relaxed);
    p.lastAppliedDef.store(-1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.lastEventCursor = -1;
    }
    p.loop.store(loop, std::memory_order_relaxed);
    p.playing.store(true, std::memory_order_release);
    InputInjector::ClearUsercmdInjections(slot);
    return true;
}

bool StopReplay(int slot)
{
    if (!ValidSlot(slot)) return false;
    g_rep[slot].playing.store(false, std::memory_order_release);
    InputInjector::ClearReplayPawn(slot);
    return true;
}

bool IsReplaying(int slot) { return ValidSlot(slot) && g_rep[slot].playing.load(std::memory_order_acquire); }

int ReplayCursor(int slot)
{
    if (!ValidSlot(slot)) return -1;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return -1;
    return p.cursor.load(std::memory_order_relaxed);
}

int ReplayTotal(int slot)
{
    if (!ValidSlot(slot)) return 0;
    ReplayState& p = g_rep[slot];
    std::lock_guard<std::mutex> lk(p.mu);
    return static_cast<int>(p.ticks.size());
}

// cursor points at the NEXT tick; the one just applied is cursor-1.
bool CurrentReplayTick(int slot, ReplayTick& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int idx = p.cursor.load(std::memory_order_relaxed) - 1;
    if (idx < 0) idx = 0;
    if (idx >= total) return false;
    out = p.ticks[idx];
    return true;
}

bool ReplayCommandViewSnapshot(int slot, MovementSnapshot& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(p.mu);
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
    if (!p.playing.load(std::memory_order_acquire)) return -1;
    std::lock_guard<std::mutex> lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int idx = p.cursor.load(std::memory_order_relaxed);
    if (idx < 0 || idx >= total) return -1;
    uint32_t begin = p.subOffset[idx];
    uint32_t end = p.subOffset[idx + 1];
    int n = static_cast<int>(end - begin);
    if (n > maxOut) n = maxOut;
    for (int i = 0; i < n; ++i)
        out[i] = p.subs[begin + i];
    return n;
}

bool CurrentReplayInputButtons(int slot, uint64_t& b0, uint64_t& b1, uint64_t& b2)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(p.mu);
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
    if (!ValidSlot(slot) || defIndex < 0 || IsReplaying(slot)) return false;
    if (!WeaponLockerHooks::WeaponHooksReady()) return false;
    void* ws = WeaponLockerHooks::WsForSlot(slot);
    if (!ws) return false;
    void* weapon = WeaponLockerHooks::FindWeaponByDef(ws, defIndex);
    if (!weapon) return false;
    return WeaponLockerHooks::SelectWeaponRaw(ws, weapon);
}

// Def index of the bot's current active weapon
int BotActiveWeaponDef(int slot)
{
    if (!ValidSlot(slot) || !WeaponLockerHooks::WeaponHooksReady()) return -1;
    void* ws = WeaponLockerHooks::WsForSlot(slot);
    if (!ws) return -1;
    return WeaponLockerHooks::ActiveWeaponDef(ws);
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
    if (!p.playing.load(std::memory_order_acquire)) return -1;
    std::lock_guard<std::mutex> lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return -1;
    return p.ticks[cur].weaponDefIndex;
}

int CurrentReplayWeaponSelect(int slot)
{
    if (!ValidSlot(slot) || !WeaponLockerHooks::WeaponHooksReady()) return -1;

    // Recorded def for the tick about to be simulated
    int recordedDef = CurrentReplayWeaponDef(slot);
    if (recordedDef < 0) return -1;

    void* ws = WeaponLockerHooks::WsForSlot(slot);
    if (!ws) return -1;

    // Already holding the recorded weapon -> no switch
    if (ReplayWeaponDefsMatch(WeaponLockerHooks::ActiveWeaponDef(ws), recordedDef))
    {
        g_rep[slot].lastAppliedDef.store(recordedDef, std::memory_order_relaxed);
        return -1;
    }

    void* weapon = FindReplayWeaponByDef(ws, recordedDef);
    if (!weapon) return -1;
    WeaponLockerHooks::SelectWeaponRaw(ws, weapon);
    g_rep[slot].lastAppliedDef.store(recordedDef, std::memory_order_relaxed);
    return WeaponLockerHooks::WeaponEntIndex(weapon);
}

// Returns the current drop event only once for each replay cursor
bool TakeCurrentReplayDrop(int slot, ReplayDropEvent& event)
{
    event = {};
    event.weaponDefIndex = -1;
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;

    std::lock_guard<std::mutex> lk(p.mu);
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= static_cast<int>(p.ticks.size()) || p.lastEventCursor == cur) return false;
    p.lastEventCursor = cur;

    const ReplayTick& tick = p.ticks[cur];
    if ((tick.eventFlags & ReplayEvent_Drop) == 0) return false;
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
    if (!ValidSlot(slot) || !services || weaponDefIndex < 0 || !IsReplaying(slot) ||
        !WeaponLockerHooks::WeaponHooksReady())
        return false;

    g_dropReplayAttemptCount.fetch_add(1, std::memory_order_relaxed);
    g_lastDropReplaySlot.store(slot, std::memory_order_relaxed);
    g_lastDropReplayWeaponDef.store(weaponDefIndex, std::memory_order_relaxed);
    g_lastDropReplayVectorFlags.store(event.vectorFlags, std::memory_order_relaxed);

    void* pawn = InputInjector::ResolveReplayPawn(slot, services);
    void* ws = nullptr;
    if (!pawn || !GuardedRead(pawn, tg::kPawn_WeaponServices, ws) || !ws) return false;
    void* weapon = WeaponLockerHooks::FindWeaponByDef(ws, weaponDefIndex);
    if (!weapon) return false;
    if (WeaponLockerHooks::ActiveWeaponDef(ws) != weaponDefIndex && !WeaponLockerHooks::SelectWeaponRaw(ws, weapon)) return false;
    if (WeaponLockerHooks::ActiveWeaponDef(ws) != weaponDefIndex) return false;

    if (!Dispatch::g_pGameClients) return false;
    CCommand command;
    if (!command.Tokenize("drop")) return false;

    g_activeReplayDropSlot = slot;
    g_activeReplayDropEvent = &event;
    Dispatch::g_pGameClients->ClientCommand(CPlayerSlot(slot), command);
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
static void WriteVelocityToPawn(int slot, void* services, const MovementSnapshot& s)
{
    void* pawn = InputInjector::ResolveReplayPawn(slot, services);
    if (!pawn) return;
    WriteVector3(pawn, tg::kEnt_AbsVelocity, s.velX, s.velY, s.velZ);
}

// Writes replay origin through the current body-component scene node.
static void WriteSceneNodeOrigin(int slot, void* services, const MovementSnapshot& s, float zBias = 0.0f)
{
    void* pawn = InputInjector::ResolveReplayPawn(slot, services);
    if (!pawn) return;

    void* node = ResolveSceneNode(pawn);
    if (!node) return;

    const float values[3] = { s.originX, s.originY, s.originZ + zBias };
    TryWriteMemoryGuarded(node, tg::kNode_AbsOrigin, values, sizeof(values));
}

// Write origin + velocity into CMoveData.
static void WriteMoveData(void* moveData, const MovementSnapshot& s)
{
    WriteVector3(moveData, tg::kMove_AbsOrigin, s.originX, s.originY, s.originZ);
    WriteVector3(moveData, tg::kMove_Velocity, s.velX, s.velY, s.velZ);
}

// Restores duck and ladder state through guarded field writes.
static void WriteMovementServiceState(void* services, const MovementSnapshot& s)
{
    WriteField(services, tg::kServices_DuckAmount, s.duckAmount);
    WriteField(services, tg::kServices_DuckSpeed, s.duckSpeed);
    WriteVector3(services, tg::kServices_LadderNormal, s.ladderNormalX, s.ladderNormalY, s.ladderNormalZ);
    WriteField(services, tg::kServices_Ducked, s.ducked);
    WriteField(services, tg::kServices_Ducking, s.ducking);
    WriteField(services, tg::kServices_DesiresDuck, s.desiresDuck);
}

// ProcessMovement (pre): seed CMoveData + pawn + moveType with pre state.
void OnReplayPre(int slot, void* services, void* moveData)
{
    if (!ValidSlot(slot) || !services || !moveData) return;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return;
    ReplayTick t{};
    {
        std::lock_guard<std::mutex> lk(p.mu);
        int total = static_cast<int>(p.ticks.size());
        int cur = p.cursor.load(std::memory_order_relaxed);
        if (cur >= total) return; // commit handler will stop/loop
        t = p.ticks[cur];
    }
    WriteMoveData(moveData, t.pre);
    WriteVelocityToPawn(slot, services, t.pre);
    WriteMovementServiceState(services, t.pre);
    // Feed recorded buttons so the engine's Duck()/ladder logic runs
    WriteField(services, tg::kServices_Buttons, t.pre.buttons);
    WriteField(services, tg::kServices_Buttons1, t.pre.buttons1);
    WriteField(services, tg::kServices_Buttons2, t.pre.buttons2);
    void* pawn = InputInjector::ResolveReplayPawn(slot, services);
    if (pawn)
    {
        WriteField(pawn, tg::kEnt_MoveType, t.pre.moveType);
        WriteSceneNodeOrigin(slot, services, t.pre);
        BotControllerHooks::ApplyReplayEyeAngles(pawn, t.pre.pitch, t.pre.yaw);
    }
}

// FinishMove (pre): write post snapshot into CMoveData + scene-node origin.
void OnReplayFinishMove(int slot, void* services, void* moveData)
{
    if (!ValidSlot(slot) || !services || !moveData) return;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return;
    ReplayTick t{};
    {
        std::lock_guard<std::mutex> lk(p.mu);
        int total = static_cast<int>(p.ticks.size());
        int cur = p.cursor.load(std::memory_order_relaxed);
        if (cur >= total) return;
        t = p.ticks[cur];
    }
    WriteMoveData(moveData, t.post);
    WriteSceneNodeOrigin(slot, services, t.post, 1000.0f);
}

void OnReplayCommit(int slot, void* services)
{
    if (!ValidSlot(slot) || !services) return;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return;

    ReplayTick t{};
    int cur, total;
    {
        std::lock_guard<std::mutex> lk(p.mu);
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
            InputInjector::ClearReplayPawn(slot);
            return;
        }
        t = p.ticks[cur];
    }

    void* pawn = InputInjector::ResolveReplayPawn(slot, services);
    if (pawn)
    {
        WriteField(pawn, tg::kEnt_MoveType, t.post.moveType);
        WriteField(pawn, tg::kEnt_ActualMoveType, t.post.actualMoveType);
        // Merge ground + ducking bits from the recording, keep the rest live.
        uint32_t live = 0;
        uint32_t mask = tg::kFL_OnGround | tg::kFL_Ducking;
        if (SafeRead(pawn, tg::kEnt_Flags, live))
        {
            live = (live & ~mask) | (t.post.entityFlags & mask);
            WriteField(pawn, tg::kEnt_Flags, live);
        }
        BotControllerHooks::ApplyReplayEyeAngles(pawn, t.post.pitch, t.post.yaw);
    }

    WriteVelocityToPawn(slot, services, t.post);
    WriteSceneNodeOrigin(slot, services, t.post);
    WriteMovementServiceState(services, t.post);

    p.cursor.store(cur + 1, std::memory_order_relaxed);
}

void ClearAll()
{
    g_dropHookReady.store(false, std::memory_order_release);
    g_hookDropWeapon.Remove();
    g_origDropWeapon = nullptr;
    g_addrDropWeapon = nullptr;
    g_dropHookTried.store(false, std::memory_order_release);
    for (int i = 0; i < kMaxSlots; ++i)
    {
        g_rec[i].recording.store(false, std::memory_order_release);
        g_rep[i].playing.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(g_rec[i].mu);
            g_rec[i].ticks.clear();
            g_rec[i].subs.clear();
            g_rec[i].pendingSubs.clear();
            g_rec[i].havePre = false;
            g_rec[i].pendingEventFlags = ReplayEvent_None;
            g_rec[i].pendingDropEvent = {};
            g_rec[i].pendingDropEvent.weaponDefIndex = -1;
            g_rec[i].pendingDropCandidates.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_rep[i].mu);
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
        InputInjector::ClearReplayPawn(i);
    }
}
} // namespace MotionRecorder
} // namespace BotController
