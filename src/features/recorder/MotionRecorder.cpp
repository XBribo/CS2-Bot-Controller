// Motion recording & replay implementation

#include "MotionRecorder.h"
#include "dispatch.h"
#include "InputInjector.h"
#include "WeaponLocker.h"
#include "ccsbot_slot.h"
#include "hooks.h"
#include "core/log.h"
#include "core/interfaces.h"
#include "offsets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <limits>
#include <vector> // NOLINT(misc-include-cleaner)

#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

namespace tg = cs2bc::offsets;

namespace cs2bc {
namespace motion_recorder {
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
    // PhysicsSimulate post that commits them to a tick.
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
    std::atomic<bool> holding{ false };
    std::atomic<bool> needsInitialTeleport{ false };
    std::atomic<bool> needsHistory{ false };
    std::atomic<bool> loop{ false };
    std::vector<ReplayFrameData> frames;
    std::vector<SubtickMove> subs;
    std::vector<uint32_t> subOffset; // prefix sum, size frames.size()+1
    std::atomic<int> cursor{ 0 };
    int startCursor{ 0 };
    int holdBeforeCursor{ -1 };
    std::atomic<int> lastAppliedDef{ -1 };
    int lastEventCursor{ -1 };
    int lastFinalViewCursor{ -1 }; // guards camera reads after the simulation cursor advances
    std::mutex mu; // guards frames/subs/subOffset
};

namespace {

std::array<RecordState, kMaxSlots> g_rec;
std::array<ReplayState, kMaxSlots> g_rep;
std::atomic<uint64_t> g_recordingSlots{ 0 };
std::atomic<uint64_t> g_replayingSlots{ 0 };
static_assert(kMaxSlots <= 64);
hooks::NativeHook<void, void*, void*, void*, const float*> g_hookDropWeapon;
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
hooks::NativeHook<char, void*, void*, int, const float*> g_hookDropReleaseOuter;
hooks::NativeHook<char, void*, void*> g_hookDropReleaseBuilder;
struct DropReleaseFrame
{
    int slot{ -1 };
    int defIndex{ -1 };
    void* weapon{ nullptr };
    void* droppedWeapon{ nullptr };
    bool haveReleasePose{ false };
    float releasePosition[3]{};
    float releaseQuaternion[4]{};
};
thread_local std::vector<DropReleaseFrame> g_dropReleaseFrames;
struct ReplayDropOverride
{
    int slot;
    void* weapon;
    ReplayDropEvent event;
};
thread_local std::vector<ReplayDropOverride> g_replayDropEvents;
constexpr int kMolotovDef = 46;
constexpr int kIncendiaryDef = 48;

bool ValidSlot(int s) { return s >= 0 && s < kMaxSlots; }
// Rejects missing or corrupted engine release transforms before replay consumes them.
bool ValidReleasePose(const float* position, const float* quaternion)
{
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(position[i])) return false;
    float norm = 0.0F;
    for (int i = 0; i < 4; ++i)
    {
        if (!std::isfinite(quaternion[i])) return false;
        norm += quaternion[i] * quaternion[i];
    }
    return norm > 0.5F && norm < 1.5F;
}
static int ReplayWeaponSelectForDef(int slot, int recordedDef);
// Publishes the frame's pawn state before the native drop command reads it.
bool PrepareReplayDropPawn(int slot, void* services, const ReplayDropEvent& event);
void* ResolveSceneNode(void* entity);

// Returns whether an item definition is either faction's fire grenade
bool IsFireGrenadeDef(int defIndex) { return defIndex == kMolotovDef || defIndex == kIncendiaryDef; }

// Prefers the recorded fire grenade
void* FindReplayWeaponByDef(void* ws, int recordedDef)
{
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, recordedDef);
    if (weapon || !IsFireGrenadeDef(recordedDef)) return weapon;

    const int alternateDef = recordedDef == kMolotovDef ? kIncendiaryDef : kMolotovDef;
    return weapon_locker_hooks::FindWeaponByDef(ws, alternateDef);
}

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

// Stores one real weapon-service drop in the pending recording tick
bool CaptureDropEvent(int slot, const ReplayDropEvent& event)
{
    if (!ValidSlot(slot) || event.weaponDefIndex < 0) return false;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return false;

    std::scoped_lock lk(r.mu);
    r.pendingEventFlags |= ReplayEventDrop;
    r.pendingDropEvent = event;
    return true;
}

// Records the drop event and item
KHook::Return<void> HookedDropWeapon(void* weaponServices, void* weapon, void*, const float*) noexcept
{
    void* pawn = nullptr;
    if (weaponServices) GuardedRead(weaponServices, tg::g_servicesPawn, pawn);
    const int recordingSlot = RecordingSlotForWeaponServices(weaponServices, pawn);
    const int weaponDefIndex = weapon_locker_hooks::ReadDefIndex(weapon);

    ReplayDropEvent recordedEvent{};
    recordedEvent.weaponDefIndex = -1;
    // Keep the raw definition for detachment checks, but record knives by role.
    if (ValidSlot(recordingSlot))
    {
        recordedEvent.weaponDefIndex =
            weapon_locker_hooks::WeaponDefForEntityIndex(weaponServices, weapon_locker_hooks::WeaponEntIndex(weapon));
        if (recordedEvent.weaponDefIndex < 0) recordedEvent.weaponDefIndex = weaponDefIndex;
        void* node = ResolveSceneNode(pawn);
        float bodyAngles[3]{};
        if (node && tg::g_nodeAbsRotation >= 0 && TryReadMemory(node, tg::g_nodeAbsRotation, bodyAngles, sizeof(bodyAngles)) &&
            std::isfinite(bodyAngles[1]))
        {
            recordedEvent.vectorFlags |= ReplayDropBodyYaw;
            recordedEvent.target[0] = bodyAngles[1];
        }
        if (!g_dropReleaseFrames.empty())
        {
            const DropReleaseFrame& frame = g_dropReleaseFrames.back();
            if (frame.droppedWeapon == weapon && frame.haveReleasePose)
            {
                recordedEvent.vectorFlags |= ReplayDropReleasePose;
                std::copy_n(frame.releasePosition, 3, recordedEvent.releasePosition);
                std::copy_n(frame.releaseQuaternion, 4, recordedEvent.releaseQuaternion);
            }
        }
    }
    g_dropFrames.push_back({ weaponServices, weapon, recordingSlot, weaponDefIndex, recordedEvent });
    return { KHook::Action::Ignore };
}

// Records only physical detachments
KHook::Return<void> DropWeaponPost(void*, void*, void*, const float*) noexcept
{
    const auto [weaponServices, weapon, recordingSlot, weaponDefIndex, recordedEvent] = g_dropFrames.back();
    g_dropFrames.pop_back();
    if (recordedEvent.weaponDefIndex < 0) return { KHook::Action::Ignore };
    const bool detached =
        weaponServices && weapon && weaponDefIndex >= 0 && weapon_locker_hooks::FindWeaponByDef(weaponServices, weaponDefIndex) != weapon;
    if (detached && ValidSlot(recordingSlot))
    {
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
    return { KHook::Action::Ignore };
}

// Installs the drop hook from a live weapon-services vtable once
void EnsureDropWeaponHook(void* weaponServices)
{
    if (!weaponServices || g_dropHookTried.exchange(true, std::memory_order_acq_rel)) return;

    void** vtable = nullptr;
    if (!GuardedRead(weaponServices, 0, vtable) || !vtable) return;
    if (!GuardedRead(static_cast<const void*>(vtable), tg::g_vtIdxDropWeapon * static_cast<int>(sizeof(void*)), g_addrDropWeapon) ||
        !g_addrDropWeapon)
        return;

    if (g_hookDropWeapon.Install(g_addrDropWeapon, &HookedDropWeapon, &DropWeaponPost))
    {
        g_dropHookReady.store(true, std::memory_order_release);
        return;
    }

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

bool WriteVector3(void* base, int offset, float x, float y, float z)
{
    const float values[3] = { x, y, z };
    return TryWriteMemory(base, offset, values, sizeof(values));
}

void* ResolveSceneNode(void* entity)
{
    void* body = nullptr;
    if (!GuardedRead(entity, tg::g_entBodyComponent, body) || !body) return nullptr;

    void* node = nullptr;
    return GuardedRead(body, tg::g_bodySceneNode, node) ? node : nullptr;
}

// Tracks the source item while the engine may split a stacked grenade into a new entity.
KHook::Return<char> DropReleaseOuterPre(void* weaponServices, void* weapon, int, const float*) noexcept
{
    DropReleaseFrame frame{};
    frame.weapon = weapon;
    frame.defIndex = weapon_locker_hooks::ReadDefIndex(weapon);
    void* pawn = nullptr;
    if (weaponServices) GuardedRead(weaponServices, tg::g_servicesPawn, pawn);
    frame.slot = RecordingSlotForWeaponServices(weaponServices, pawn);
    if (!ValidSlot(frame.slot))
    {
        frame.slot = -1;
        for (int slot = 0; slot < kMaxSlots; ++slot)
        {
            if (IsReplaying(slot) && weapon_locker_hooks::WsForSlot(slot) == weaponServices)
            {
                frame.slot = slot;
                break;
            }
        }
    }
    g_dropReleaseFrames.push_back(frame);
    return { KHook::Action::Ignore };
}

// Balances the per-thread drop frame after the native outer call returns.
KHook::Return<char> DropReleaseOuterPost(void*, void*, int, const float*) noexcept
{
    if (!g_dropReleaseFrames.empty()) g_dropReleaseFrames.pop_back();
    return { KHook::Action::Ignore };
}

// Associates the transform builder with the actual item, including split grenades.
KHook::Return<char> DropReleaseBuilderPre(void* weapon, void*) noexcept
{
    if (!g_dropReleaseFrames.empty())
    {
        DropReleaseFrame& frame = g_dropReleaseFrames.back();
        if (frame.slot >= 0 && weapon_locker_hooks::ReadDefIndex(weapon) == frame.defIndex) frame.droppedWeapon = weapon;
    }
    return { KHook::Action::Ignore };
}

// Captures the release pose on recording or applies it to the matching replay drop.
KHook::Return<char> DropReleaseBuilderPost(void* weapon, void* transform) noexcept
{
    if (g_dropReleaseFrames.empty()) return { KHook::Action::Ignore };
    DropReleaseFrame& frame = g_dropReleaseFrames.back();
    if (frame.slot < 0 || frame.droppedWeapon != weapon) return { KHook::Action::Ignore };
    float output[8]{};
    const bool readable = transform && TryReadMemoryGuarded(transform, 0, output, sizeof(output));
    if (readable && IsRecording(frame.slot) && ValidReleasePose(output, output + 4))
    {
        std::copy_n(output, 3, frame.releasePosition);
        std::copy_n(output + 4, 4, frame.releaseQuaternion);
        frame.haveReleasePose = true;
    }
    else if (readable && IsReplaying(frame.slot) && !g_replayDropEvents.empty())
    {
        const ReplayDropOverride& override = g_replayDropEvents.back();
        const ReplayDropEvent& event = override.event;
        if (override.slot == frame.slot && override.weapon == frame.weapon && (event.vectorFlags & ReplayDropReleasePose) != 0)
        {
            std::copy_n(event.releasePosition, 3, output);
            std::copy_n(event.releaseQuaternion, 4, output + 4);
            if (!TryWriteMemoryGuarded(transform, 0, output, sizeof(output)))
                BC_LOG_WARN("Drop release pose override failed for slot %d\n", frame.slot);
        }
    }
    return { KHook::Action::Ignore };
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

// Installs the release-pose hooks used by both recording and replay.
bool InstallDropReleasePose(void* outerDrop, void* buildTransform)
{
    if (!outerDrop || !buildTransform) return false;
    if (!g_hookDropReleaseBuilder.Install(buildTransform, &DropReleaseBuilderPre, &DropReleaseBuilderPost) ||
        !g_hookDropReleaseOuter.Install(outerDrop, &DropReleaseOuterPre, &DropReleaseOuterPost))
    {
        g_hookDropReleaseOuter.Remove();
        g_hookDropReleaseBuilder.Remove();
        return false;
    }
    return true;
}

bool StartRecord(int slot)
{
    if (!ValidSlot(slot) || !input_injector::RecorderReady()) return false;
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
    g_recordingSlots.fetch_or(uint64_t{ 1 } << slot, std::memory_order_release);
    r.recording.store(true, std::memory_order_release);
    return true;
}

bool StopRecord(int slot)
{
    if (!ValidSlot(slot)) return false;
    g_rec[slot].recording.store(false, std::memory_order_release);
    g_recordingSlots.fetch_and(~(uint64_t{ 1 } << slot), std::memory_order_release);
    return true;
}

bool IsRecording(int slot) { return ValidSlot(slot) && g_rec[slot].recording.load(std::memory_order_acquire); }

// Skips recording-only work without scanning per-slot state.
bool HasAnyRecording() { return g_recordingSlots.load(std::memory_order_acquire) != 0; }

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
        t.eventDropReleaseX = r.pendingDropEvent.releasePosition[0];
        t.eventDropReleaseY = r.pendingDropEvent.releasePosition[1];
        t.eventDropReleaseZ = r.pendingDropEvent.releasePosition[2];
        t.eventDropReleaseQuatX = r.pendingDropEvent.releaseQuaternion[0];
        t.eventDropReleaseQuatY = r.pendingDropEvent.releaseQuaternion[1];
        t.eventDropReleaseQuatZ = r.pendingDropEvent.releaseQuaternion[2];
        t.eventDropReleaseQuatW = r.pendingDropEvent.releaseQuaternion[3];
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

int CopyTicks(int slot, ReplayTick* out, int maxTicks) { return CopyTicksRange(slot, 0, out, maxTicks); }

// Copies one bounded range without locking for the full recording length.
int CopyTicksRange(int slot, int start, ReplayTick* out, int maxTicks)
{
    if (!ValidSlot(slot) || start < 0 || !out || maxTicks <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    int n = std::min(maxTicks, std::max(0, static_cast<int>(r.ticks.size()) - start));
    for (int i = 0; i < n; ++i)
        out[i] = r.ticks[start + i];
    return n;
}

int CopySubticks(int slot, SubtickMove* out, int maxSubticks) { return CopySubticksRange(slot, 0, out, maxSubticks); }

// Copies one bounded subtick range from the stopped recording.
int CopySubticksRange(int slot, int start, SubtickMove* out, int maxSubticks)
{
    if (!ValidSlot(slot) || start < 0 || !out || maxSubticks <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    int n = std::min(maxSubticks, std::max(0, static_cast<int>(r.subs.size()) - start));
    for (int i = 0; i < n; ++i)
        out[i] = r.subs[start + i];
    return n;
}

// Copies captured command frames into a caller-owned buffer
int CopyCommands(int slot, ReplayCommandFrameData* out, int maxCommands) { return CopyCommandsRange(slot, 0, out, maxCommands); }

// Copies one bounded command range from the stopped recording.
int CopyCommandsRange(int slot, int start, ReplayCommandFrameData* out, int maxCommands)
{
    if (!ValidSlot(slot) || start < 0 || !out || maxCommands <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    int n = std::min(maxCommands, std::max(0, static_cast<int>(r.commands.size()) - start));
    for (int i = 0; i < n; ++i)
        out[i] = r.commands[start + i];
    return n;
}

// ---- replay ----

namespace {
// Keeps complete engine planes intact; legacy held-only frames use the adjacent-state encoding.
void ReplayButtonsAt(const ReplayState& replay, int cursor, uint64_t& b0, uint64_t& b1, uint64_t& b2)
{
    const ReplayCommandFrameData* command = &replay.frames[cursor].input;
    if (command && (command->fields & kCommandFieldButtons) != 0)
    {
        b0 = command->buttons;
        b1 = command->buttons1;
        b2 = command->buttons2;
        return;
    }
    const MovementSnapshot& pre = replay.frames[cursor].tick.pre;
    b0 = pre.buttons;
    b1 = pre.buttons1;
    b2 = pre.buttons2;
    if (b1 != 0 || b2 != 0) return;
    uint64_t previousHeld = 0;
    if (cursor > 0)
    {
        const auto* previous = &replay.frames[cursor - 1].input;
        previousHeld = previous && (previous->fields & kCommandFieldButtons) != 0 ? previous->buttons : replay.frames[cursor - 1].tick.pre.buttons;
    }
    b1 = b0 ^ previousHeld;
    b2 = 0;
}

// Rejects malformed selected subtick fractions while leaving absent fields unrestricted.
bool ValidReplayFraction(float fraction) { return std::isfinite(fraction) && fraction >= 0.0F && fraction < 1.0F; }

// Validates the presence mask and selected data before replacing any loaded buffer.
bool ValidReplayHistory(const ReplayHistoryData& extra)
{
    const uint32_t fields = extra.fields;
    if ((fields & ~ReplayHistoryAll) != 0) return false;
    if (fields == 0) return true;
    if (extra.sourcePlayerTick < 0 || !std::isfinite(extra.sourceTickrate) || extra.sourceTickrate <= 0.0F) return false;
    if ((fields & ReplayHistoryJumpPressedTime) != 0 && !std::isfinite(extra.jumpPressedTime)) return false;
    if ((fields & ReplayHistoryLastDuckTime) != 0 && !std::isfinite(extra.lastDuckTime)) return false;
    if ((fields & ReplayHistoryLastActualJumpPress) != 0 && !ValidReplayFraction(extra.lastActualJumpPressFrac)) return false;
    if ((fields & ReplayHistoryLastUsableJumpPress) != 0 && !ValidReplayFraction(extra.lastUsableJumpPressFrac)) return false;
    if ((fields & ReplayHistoryLastLanded) != 0 && !ValidReplayFraction(extra.lastLandedFrac)) return false;
    return (fields & ReplayHistoryLastLandedVelocity) == 0 ||
           (std::isfinite(extra.lastLandedVelocityX) && std::isfinite(extra.lastLandedVelocityY) && std::isfinite(extra.lastLandedVelocityZ));
}
} // namespace

// Validate, stage, and atomically replace one frame buffer and its subtick storage.
bool LoadReplay(int slot, float tickRate, const ReplayFrameData* frames, int frameCount,
                const SubtickMove* subs, int subCount) noexcept
{
    try
    {
        if (!ValidSlot(slot) || !frames || frameCount <= 0 || !std::isfinite(tickRate) || tickRate < 0.0F ||
            subCount < 0 || (subCount > 0 && !subs)) return false;
        ReplayState& p = g_rep[slot];
        if (p.playing.load(std::memory_order_acquire)) return false;
        std::vector<uint32_t> stagedOffsets(static_cast<size_t>(frameCount) + 1, 0);
        uint64_t totalSubticks = 0;
        for (int i = 0; i < frameCount; ++i)
        {
            auto history = frames[i].history;
            history.sourceTickrate = tickRate;
            if (!ValidReplayHistory(history)) return false;
            const auto& tick = frames[i].tick;
            if (tick.numSubtick > kMaxSubtickPerTick) return false;
            if ((tick.eventFlags & ReplayEventDrop) != 0)
            {
                const float position[3] = { tick.eventDropReleaseX, tick.eventDropReleaseY, tick.eventDropReleaseZ };
                const float quaternion[4] = { tick.eventDropReleaseQuatX, tick.eventDropReleaseQuatY,
                                              tick.eventDropReleaseQuatZ, tick.eventDropReleaseQuatW };
                if ((tick.eventDropVectorFlags & ReplayDropReleasePose) == 0 || !ValidReleasePose(position, quaternion)) return false;
            }
            stagedOffsets[static_cast<size_t>(i)] = static_cast<uint32_t>(totalSubticks);
            totalSubticks += tick.numSubtick;
            if (totalSubticks > static_cast<uint64_t>(subCount)) return false;
        }
        if (totalSubticks != static_cast<uint64_t>(subCount)) return false;
        stagedOffsets[static_cast<size_t>(frameCount)] = static_cast<uint32_t>(totalSubticks);
        std::vector<ReplayFrameData> stagedFrames(frames, frames + frameCount);
        for (auto& frame : stagedFrames) frame.history.sourceTickrate = tickRate;
        std::vector<SubtickMove> stagedSubs;
        if (subCount > 0) stagedSubs.assign(subs, subs + subCount);

        std::scoped_lock lk(p.mu);
        if (p.playing.load(std::memory_order_acquire)) return false;
        p.frames.swap(stagedFrames);
        p.subs.swap(stagedSubs);
        p.subOffset.swap(stagedOffsets);
        p.cursor.store(0, std::memory_order_relaxed);
        p.startCursor = 0;
        p.holdBeforeCursor = -1;
        p.holding.store(false, std::memory_order_release);
        p.needsHistory.store(false, std::memory_order_release);
        p.lastAppliedDef.store(-1, std::memory_order_relaxed);
        p.lastEventCursor = -1;
        p.lastFinalViewCursor = -1;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

namespace {
// Shares range validation and initialization while preserving a held replay's physical state.
bool StartReplayRange(int slot, bool loop, int startIndex, int holdBeforeIndex)
{
    if (!ValidSlot(slot) || !input_injector::ReplayReady()) return false;
    if (tg::g_vtIdxTeleport < 0)
    {
        BC_LOG_WARN("Cannot start replay: CBaseEntity_Teleport gamedata is missing\n");
        return false;
    }
    ReplayState& p = g_rep[slot];
    {
        std::scoped_lock lk(p.mu);
        const int total = static_cast<int>(p.frames.size());
        if (startIndex < 0 || startIndex >= total ||
            (holdBeforeIndex != -1 && (holdBeforeIndex <= startIndex || holdBeforeIndex > total)))
            return false;
        const bool resumesHold = holdBeforeIndex == -1 && p.playing.load(std::memory_order_acquire) &&
                                 p.holding.load(std::memory_order_acquire) &&
                                 p.cursor.load(std::memory_order_relaxed) == startIndex;
        p.cursor.store(startIndex, std::memory_order_relaxed);
        // Releasing a pre-roll hold continues the same replay, including its original loop start.
        if (!resumesHold)
        {
            p.startCursor = startIndex;
            p.lastFinalViewCursor = -1;
            p.needsInitialTeleport.store(true, std::memory_order_release);
        }
        p.holdBeforeCursor = holdBeforeIndex;
        // A resumed hold only rebases history; its engine-generated pose is not teleported.
        p.needsHistory.store(true, std::memory_order_release);
        p.holding.store(false, std::memory_order_release);
        p.lastAppliedDef.store(-1, std::memory_order_relaxed);
        p.lastEventCursor = -1;
        p.loop.store(loop, std::memory_order_relaxed);
        g_replayingSlots.fetch_or(uint64_t{ 1 } << slot, std::memory_order_release);
        p.playing.store(true, std::memory_order_release);
    }
    input_injector::ClearUsercmdInjections(slot);
    return true;
}
} // namespace

// Preserves the original start-from-zero API.
bool StartReplay(int slot, bool loop) { return StartReplayAt(slot, loop, 0); }

// Starts or resumes replay at an inclusive input index.
bool StartReplayAt(int slot, bool loop, int startIndex) { return StartReplayRange(slot, loop, startIndex, -1); }

// Stops consuming input before the exclusive boundary while keeping the pawn under replay control.
bool StartReplayUntil(int slot, bool loop, int startIndex, int holdBeforeIndex)
{
    if (holdBeforeIndex < 0) return false;
    return StartReplayRange(slot, loop, startIndex, holdBeforeIndex);
}

bool StopReplay(int slot)
{
    if (!ValidSlot(slot)) return false;
    g_rep[slot].playing.store(false, std::memory_order_release);
    g_rep[slot].holding.store(false, std::memory_order_release);
    g_rep[slot].needsHistory.store(false, std::memory_order_release);
    g_replayingSlots.fetch_and(~(uint64_t{ 1 } << slot), std::memory_order_release);
    input_injector::ClearReplayPawn(slot);
    return true;
}

// Releases vector capacities as well as stopping replay; recorded buffers are unaffected.
bool ReleaseReplayBuffer(int slot)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    std::scoped_lock lk(p.mu);
    StopReplay(slot);
    std::vector<ReplayFrameData>{}.swap(p.frames);
    std::vector<SubtickMove>{}.swap(p.subs);
    std::vector<uint32_t>{}.swap(p.subOffset);
    p.cursor.store(0, std::memory_order_relaxed);
    p.startCursor = 0;
    p.holdBeforeCursor = -1;
    p.loop.store(false, std::memory_order_relaxed);
    p.needsInitialTeleport.store(false, std::memory_order_release);
    p.needsHistory.store(false, std::memory_order_release);
    p.lastAppliedDef.store(-1, std::memory_order_relaxed);
    p.lastEventCursor = -1;
    p.lastFinalViewCursor = -1;
    return true;
}

bool IsReplaying(int slot) { return ValidSlot(slot) && g_rep[slot].playing.load(std::memory_order_acquire); }

// Allows the input hook to suppress execution without replaying the last command.
bool IsReplayHolding(int slot)
{
    return IsReplaying(slot) && g_rep[slot].holding.load(std::memory_order_acquire);
}

// Skips replay-only work without dereferencing engine objects.
bool HasAnyReplay() { return g_replayingSlots.load(std::memory_order_acquire) != 0; }

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
    std::scoped_lock lk(p.mu);
    return static_cast<int>(p.frames.size());
}

// Reports the last completed tick only while replay owns the slot.
bool GetReplaySlotState(int slot, ReplaySlotState& out)
{
    out = { 0, -1, 0, -1, -1, 0 };
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    std::scoped_lock lk(p.mu);
    out.playing = p.playing.load(std::memory_order_acquire) ? 1 : 0;
    out.cursor = p.cursor.load(std::memory_order_relaxed);
    out.total = static_cast<int32_t>(p.frames.size());
    if (!out.playing) return true;
    const int index = std::max(out.cursor - 1, p.startCursor);
    if (index >= out.total) return true;
    const auto& tick = p.frames[index].tick;
    out.currentTickIndex = index;
    out.weaponDefIndex = tick.weaponDefIndex;
    out.numSubtick = static_cast<int32_t>(tick.numSubtick);
    return true;
}

// cursor points at the NEXT tick; the one just applied is cursor-1.
bool CurrentReplayTick(int slot, ReplayTick& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.frames.size());
    int idx = p.cursor.load(std::memory_order_relaxed) - 1;
    idx = std::max(idx, p.startCursor);
    if (idx >= total) return false;
    out = p.frames[idx].tick;
    return true;
}

// Query one complete public frame without mixing cursor generations or losing presence.
bool CurrentReplayFrame(int slot, ReplayFrameData& out, SubtickMove* subs, int maxSubticks)
{
    if (!ValidSlot(slot) || maxSubticks < 0) return false;
    ReplayState& p = g_rep[slot];
    std::scoped_lock lk(p.mu);
    if (!p.playing.load(std::memory_order_acquire)) return false;
    const int index = std::max(p.cursor.load(std::memory_order_relaxed) - 1, p.startCursor);
    if (index < 0 || static_cast<size_t>(index) >= p.frames.size() || p.subOffset.size() != p.frames.size() + 1) return false;
    const auto& tick = p.frames[index].tick;
    if (tick.numSubtick > static_cast<uint32_t>(maxSubticks) || (tick.numSubtick > 0 && !subs)) return false;
    out = p.frames[index];
    if (tick.numSubtick > 0)
        std::copy_n(p.subs.data() + p.subOffset[index], tick.numSubtick, subs);
    return true;
}

// Assembles PlayerRunCommand input from one stored replay frame and its subticks.
bool ReplayCommandFrameForSimulation(int slot, ReplayCommandFrame& out)
{
    out = {};
    out.weaponSelect = -1;
    out.rawWeaponSelect = -1;
    if (!ValidSlot(slot)) return false;

    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;

    int recordedDef = -1;
    bool recordedWeaponChanged = false;
    {
        std::scoped_lock lk(p.mu);
        const int total = static_cast<int>(p.frames.size());
        const int cur = p.cursor.load(std::memory_order_relaxed);
        if (p.holding.load(std::memory_order_acquire)) return false;
        if (cur < 0 || cur >= total || p.subOffset.size() != p.frames.size() + 1) return false;

        out.tick = p.frames[static_cast<size_t>(cur)].tick;
        recordedDef = out.tick.weaponDefIndex;
        recordedWeaponChanged = cur > 0 && !ReplayWeaponDefsMatch(p.frames[static_cast<size_t>(cur - 1)].tick.weaponDefIndex, recordedDef);
        out.commandView = out.tick.pre;
        ReplayButtonsAt(p, cur, out.buttons0, out.buttons1, out.buttons2);

        const ReplayCommandFrameData* command =
            &p.frames[static_cast<size_t>(cur)].input;
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

    // Replay requests
    if (out.rawWeaponSelect > 0)
    {
        // Recorded entity indices cannot identify entities in the live replay server.
        const int requestedDef = (out.commandFields & kCommandFieldWeaponSelectDef) != 0 ? out.rawWeaponSelect : recordedDef;
        void* weapon = FindReplayWeaponByDef(weapon_locker_hooks::WsForSlot(slot), requestedDef);
        if (weapon) out.weaponSelect = weapon_locker_hooks::WeaponEntIndex(weapon);
    }
    else if (p.needsInitialTeleport.load(std::memory_order_acquire) || recordedWeaponChanged)
    {
        // Preserve selections issued through client commands rather than UserCmd.
        out.weaponSelect = ReplayWeaponSelectForDef(slot, recordedDef);
    }
    return true;
}

bool ReplayCommandViewSnapshot(int slot, MovementSnapshot& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.frames.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return false;
    out = p.frames[cur].tick.pre;
    return true;
}

// Mirrors DemoTracer's getter view: keep the last completed post view until FinishMove prepares another.
bool ReplaySpectatorView(int slot, MovementSnapshot& out)
{
    if (!ValidSlot(slot) || !IsReplaying(slot)) return false;
    ReplayState& p = g_rep[slot];
    std::scoped_lock lk(p.mu);
    const int total = static_cast<int>(p.frames.size());
    const int cur = p.cursor.load(std::memory_order_relaxed);
    const int lastFinal = p.lastFinalViewCursor;
    int idx = cur;
    if (lastFinal >= 0 && lastFinal < total && (lastFinal == cur || lastFinal + 1 == cur || cur >= total)) idx = lastFinal;
    if (idx < 0 || idx >= total) return false;
    out = p.frames[static_cast<size_t>(idx)].tick.post;
    return true;
}

int CurrentReplaySubticks(int slot, SubtickMove* out, int maxOut)
{
    if (!ValidSlot(slot) || !out || maxOut <= 0) return -1;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return -1;
    std::scoped_lock lk(p.mu);
    if (p.holding.load(std::memory_order_acquire)) return 0;
    int total = static_cast<int>(p.frames.size());
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
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::scoped_lock lk(p.mu);
    if (p.holding.load(std::memory_order_acquire))
    {
        b0 = b1 = b2 = 0;
        return true;
    }
    int total = static_cast<int>(p.frames.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return false;
    ReplayButtonsAt(p, cur, b0, b1, b2);
    return true;
}

bool SwitchBotWeaponByDef(int slot, int defIndex)
{
    if (!ValidSlot(slot) || defIndex < 0 || IsReplaying(slot)) return false;
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
    if (!ValidSlot(slot) || !weapon_locker_hooks::WeaponHooksReady()) return -1;
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
    if (!p.playing.load(std::memory_order_acquire)) return -1;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.frames.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return -1;
    return p.frames[cur].tick.weaponDefIndex;
}

// Resolves and applies the recorded weapon definition for one command frame
namespace {

int ReplayWeaponSelectForDef(int slot, int recordedDef)
{
    if (!ValidSlot(slot) || !weapon_locker_hooks::WeaponHooksReady()) return -1;
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
    if (!p.playing.load(std::memory_order_acquire)) return false;

    std::scoped_lock lk(p.mu);
    if (p.holding.load(std::memory_order_acquire)) return false;
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || static_cast<size_t>(cur) >= p.frames.size() || p.lastEventCursor == cur) return false;
    p.lastEventCursor = cur;

    const ReplayTick& tick = p.frames[cur].tick;
    if ((tick.eventFlags & ReplayEventDrop) == 0) return false;
    event.weaponDefIndex = tick.eventWeaponDefIndex;
    event.vectorFlags = tick.eventDropVectorFlags;
    event.target[0] = tick.eventDropTargetX;
    event.releasePosition[0] = tick.eventDropReleaseX;
    event.releasePosition[1] = tick.eventDropReleaseY;
    event.releasePosition[2] = tick.eventDropReleaseZ;
    event.releaseQuaternion[0] = tick.eventDropReleaseQuatX;
    event.releaseQuaternion[1] = tick.eventDropReleaseQuatY;
    event.releaseQuaternion[2] = tick.eventDropReleaseQuatZ;
    event.releaseQuaternion[3] = tick.eventDropReleaseQuatW;
    return true;
}

// Dispatches the same client command path used when a player presses G
bool DropReplayEventWeapon(int slot, void* services, const ReplayDropEvent& event)
{
    const int weaponDefIndex = event.weaponDefIndex;
    if (!ValidSlot(slot) || !services || weaponDefIndex < 0 || !IsReplaying(slot) || !weapon_locker_hooks::WeaponHooksReady() ||
        !g_hookDropReleaseBuilder.Active() || (event.vectorFlags & ReplayDropReleasePose) == 0)
        return false;

    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    void* ws = nullptr;
    if (!pawn || !GuardedRead(pawn, tg::g_pawnWeaponServices, ws) || !ws) return false;
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, weaponDefIndex);
    if (!weapon) return false;
    const int replayDef = weapon_locker_hooks::WeaponDefForEntityIndex(ws, weapon_locker_hooks::WeaponEntIndex(weapon));
    if (replayDef != weaponDefIndex) return false;
    if (weapon_locker_hooks::ActiveWeaponDef(ws) != weaponDefIndex && !weapon_locker_hooks::SelectWeaponRaw(ws, weapon)) return false;
    if (weapon_locker_hooks::ActiveWeaponDef(ws) != weaponDefIndex) return false;

    if (!dispatch::g_gameClients) return false;
    CCommand command;
    if (!command.Tokenize("drop")) return false;

    if (!PrepareReplayDropPawn(slot, services, event)) return false;
    g_replayDropEvents.push_back({ slot, weapon, event });
    dispatch::g_gameClients->ClientCommand(CPlayerSlot(slot), command);
    g_replayDropEvents.pop_back();
    return true;
}

// Reports whether the native weapon-service drop hook is installed
bool DropHookReady() { return g_dropHookReady.load(std::memory_order_acquire); }

namespace {

struct HistoryWrite
{
    uint32_t field;
    int offset;
    uint32_t bits;
};

// Preserves inactive tick sentinels and checks the live-clock translation for overflow.
bool RebaseHistoryTick(int32_t& tick, int32_t sourceTick, int32_t liveTick)
{
    if (tick <= 0) return true;
    const int64_t mapped = static_cast<int64_t>(tick) - sourceTick + liveTick;
    if (mapped < std::numeric_limits<int32_t>::min() || mapped > std::numeric_limits<int32_t>::max()) return false;
    tick = static_cast<int32_t>(mapped);
    return true;
}

// Preserves inactive second-based sentinels while translating active times to the command clock.
bool RebaseHistoryTime(float& time, double sourceTime, double liveTime)
{
    if (time <= 0.0F) return true;
    const double mapped = static_cast<double>(time) - sourceTime + liveTime;
    if (!std::isfinite(mapped) || std::fabs(mapped) > std::numeric_limits<float>::max()) return false;
    time = static_cast<float>(mapped);
    return true;
}

// Prepares every selected value and Schema target before initial teleport or history mutation.
bool PrepareHistoryWrites(int slot, ReplayHistoryData extra, std::array<HistoryWrite, 11>& writes)
{
    writes = {};
    if (extra.fields == 0) return true;
    constexpr uint32_t timeFields = ReplayHistoryJumpPressedTime | ReplayHistoryLastDuckTime |
                                    ReplayHistoryLastActualJumpPress | ReplayHistoryLastUsableJumpPress | ReplayHistoryLastLanded;
    if ((extra.fields & timeFields) != 0)
    {
        int32_t liveTick = 0;
        const float interval = interfaces::TickInterval();
        if (!input_injector::ReadReplayTickBase(slot, liveTick) || !std::isfinite(interval) || interval <= 0.0F ||
            std::fabs(static_cast<double>(extra.sourceTickrate) * interval - 1.0) > 0.0001)
        {
            BC_LOG_WARN("Cannot restore movement history for slot %d: player clock unavailable or tickrates differ\n", slot);
            return false;
        }
        const double sourceTime = static_cast<double>(extra.sourcePlayerTick) / extra.sourceTickrate;
        const double liveTime = static_cast<double>(liveTick) * interval;
        if (((extra.fields & ReplayHistoryJumpPressedTime) != 0 && !RebaseHistoryTime(extra.jumpPressedTime, sourceTime, liveTime)) ||
            ((extra.fields & ReplayHistoryLastDuckTime) != 0 && !RebaseHistoryTime(extra.lastDuckTime, sourceTime, liveTime)) ||
            ((extra.fields & ReplayHistoryLastActualJumpPress) != 0 && !RebaseHistoryTick(extra.lastActualJumpPressTick, extra.sourcePlayerTick, liveTick)) ||
            ((extra.fields & ReplayHistoryLastUsableJumpPress) != 0 && !RebaseHistoryTick(extra.lastUsableJumpPressTick, extra.sourcePlayerTick, liveTick)) ||
            ((extra.fields & ReplayHistoryLastLanded) != 0 && !RebaseHistoryTick(extra.lastLandedTick, extra.sourcePlayerTick, liveTick)))
        {
            BC_LOG_WARN("Cannot restore movement history for slot %d: clock translation overflow\n", slot);
            return false;
        }
    }
    writes = {{
        { ReplayHistoryJumpPressedTime, tg::g_servicesJumpPressedTime, std::bit_cast<uint32_t>(extra.jumpPressedTime) },
        { ReplayHistoryLastDuckTime, tg::g_servicesLastDuckTime, std::bit_cast<uint32_t>(extra.lastDuckTime) },
        { ReplayHistoryLastActualJumpPress, tg::g_servicesLastActualJumpPressTick, std::bit_cast<uint32_t>(extra.lastActualJumpPressTick) },
        { ReplayHistoryLastActualJumpPress, tg::g_servicesLastActualJumpPressFrac, std::bit_cast<uint32_t>(extra.lastActualJumpPressFrac) },
        { ReplayHistoryLastUsableJumpPress, tg::g_servicesLastUsableJumpPressTick, std::bit_cast<uint32_t>(extra.lastUsableJumpPressTick) },
        { ReplayHistoryLastUsableJumpPress, tg::g_servicesLastUsableJumpPressFrac, std::bit_cast<uint32_t>(extra.lastUsableJumpPressFrac) },
        { ReplayHistoryLastLanded, tg::g_servicesLastLandedTick, std::bit_cast<uint32_t>(extra.lastLandedTick) },
        { ReplayHistoryLastLanded, tg::g_servicesLastLandedFrac, std::bit_cast<uint32_t>(extra.lastLandedFrac) },
        { ReplayHistoryLastLandedVelocity, tg::g_servicesLastLandedVelocityX, std::bit_cast<uint32_t>(extra.lastLandedVelocityX) },
        { ReplayHistoryLastLandedVelocity, tg::g_servicesLastLandedVelocityY, std::bit_cast<uint32_t>(extra.lastLandedVelocityY) },
        { ReplayHistoryLastLandedVelocity, tg::g_servicesLastLandedVelocityZ, std::bit_cast<uint32_t>(extra.lastLandedVelocityZ) },
    }};
    for (auto& write : writes)
    {
        write.field &= extra.fields;
        if (write.field != 0 && write.offset < 0)
        {
            BC_LOG_WARN("Cannot restore movement history for slot %d: Schema field unavailable for mask 0x%x\n", slot, write.field);
            return false;
        }
    }
    return true;
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

// Writes simulation-local view; the getter leaves network eye-angle publication to the engine.
bool WriteLocalViewAnglesToPawn(void* pawn, float pitch, float yaw)
{
    const float normalizedYaw = NormalizeReplayYaw(yaw);
    return WriteVector3(pawn, tg::g_pawnViewAngle, pitch, normalizedYaw, 0.0F);
}

// Seeds view history for native command and final-view publication.
void WriteReplayViewHistory(void* services, void* pawn, float pitch, float yaw)
{
    const float normalizedYaw = NormalizeReplayYaw(yaw);
    WriteVector3(pawn, tg::g_pawnViewAnglePrevious, pitch, normalizedYaw, 0.0F);
    WriteVector3(services, tg::g_servicesOldViewAngles, pitch, normalizedYaw, 0.0F);
}

// Restores drop inputs without consuming the command's initial-position or seeded state.
bool PrepareReplayDropPawn(int slot, void* services, const ReplayDropEvent& event)
{
    MovementSnapshot pre{};
    if (!services || !ReplayCommandViewSnapshot(slot, pre)) return false;
    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    void** vtable = nullptr;
    void* target = nullptr;
    if (!pawn || tg::g_vtIdxTeleport < 0 || !SafeRead(pawn, 0, vtable) || !vtable ||
        !SafeRead(vtable, tg::g_vtIdxTeleport * static_cast<int>(sizeof(void*)), target) || !target)
        return false;

    // Teleport updates engine spatial state even for a stationary or first-frame drop.
    const float position[3] = { pre.originX, pre.originY, pre.originZ };
    const float velocity[3] = { pre.velX, pre.velY, pre.velZ };
    // Align grounded idle body facing with the recorded drop pose before the native bone read.
    const bool alignStationaryBody = (pre.entityFlags & tg::kFlOnGround) != 0 && std::fabs(pre.velX) < 0.01F && std::fabs(pre.velY) < 0.01F;
    const bool hasRecordedBodyYaw = (event.vectorFlags & ReplayDropBodyYaw) != 0 && std::isfinite(event.target[0]);
    const float bodyYaw = hasRecordedBodyYaw ? event.target[0] : pre.yaw;
    const float angles[3] = { 0.0F, NormalizeReplayYaw(bodyYaw), 0.0F };
    const float* teleportAngles = alignStationaryBody ? angles : nullptr;
    using TeleportFn = void(BC_FASTCALL*)(void*, const float*, const float*, const float*);
    reinterpret_cast<TeleportFn>(target)(pawn, position, teleportAngles, velocity);
    if (!IsReplaying(slot)) return false;
    pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn) return false;

    WriteMovementServiceState(services, pre);
    WriteField(pawn, tg::g_entMoveType, pre.moveType);
    WriteField(pawn, tg::g_entActualMoveType, pre.actualMoveType);
    uint32_t flags = 0;
    if (SafeRead(pawn, tg::g_entFlags, flags))
    {
        const uint32_t mask = tg::kFlOnGround | tg::kFlDucking;
        WriteField(pawn, tg::g_entFlags, (flags & ~mask) | (pre.entityFlags & mask));
    }
    return WriteLocalViewAnglesToPawn(pawn, pre.pitch, pre.yaw);
}
} // namespace

// Initializes at start/seek/loop, or rebases history after a hold; subsequent movement is engine-owned.
bool OnReplayCommandPre(int slot, void* services, const ReplayTick& tick, const MovementSnapshot& commandView)
{
    if (!ValidSlot(slot) || !services || !IsReplaying(slot)) return false;

    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn)
    {
        StopReplay(slot);
        return false;
    }
    ReplayState& p = g_rep[slot];
    std::array<HistoryWrite, 11> extraWrites{};
    if (p.needsHistory.exchange(false, std::memory_order_acq_rel))
    {
        ReplayHistoryData extra{};
        {
            std::scoped_lock lk(p.mu);
            const int cur = p.cursor.load(std::memory_order_relaxed);
            if (cur >= 0 && static_cast<size_t>(cur) < p.frames.size()) extra = p.frames[cur].history;
        }
        if (!PrepareHistoryWrites(slot, extra, extraWrites))
        {
            StopReplay(slot);
            return false;
        }
    }
    if (p.needsInitialTeleport.exchange(false, std::memory_order_acq_rel))
    {
        // Publish the initial origin through the engine even when movement is idle.
        void** vtable = nullptr;
        void* target = nullptr;
        if (tg::g_vtIdxTeleport < 0 || !SafeRead(pawn, 0, vtable) || !vtable ||
            !SafeRead(vtable, tg::g_vtIdxTeleport * static_cast<int>(sizeof(void*)), target) || !target)
        {
            BC_LOG_WARN("Cannot position replay pawn for slot %d: Teleport unavailable\n", slot);
            StopReplay(slot);
            return false;
        }
        const float position[3] = { tick.pre.originX, tick.pre.originY, tick.pre.originZ };
        const float velocity[3] = { tick.pre.velX, tick.pre.velY, tick.pre.velZ };
        using TeleportFn = void(BC_FASTCALL*)(void*, const float*, const float*, const float*);
        reinterpret_cast<TeleportFn>(target)(pawn, position, nullptr, velocity);
        if (!IsReplaying(slot)) return false;
        pawn = input_injector::ResolveReplayPawn(slot, services);
        if (!pawn)
        {
            StopReplay(slot);
            return false;
        }
        WriteMovementServiceState(services, tick.pre);
        WriteField(pawn, tg::g_entMoveType, tick.pre.moveType);
        WriteField(pawn, tg::g_entActualMoveType, tick.pre.actualMoveType);
        uint32_t flags = 0;
        if (SafeRead(pawn, tg::g_entFlags, flags))
        {
            const uint32_t mask = tg::kFlOnGround | tg::kFlDucking;
            WriteField(pawn, tg::g_entFlags, (flags & ~mask) | (tick.pre.entityFlags & mask));
        }
    }
    for (const auto& write : extraWrites)
    {
        if (write.field != 0 && !WriteField(services, write.offset, write.bits))
        {
            BC_LOG_WARN("Cannot restore movement history for slot %d: field write failed\n", slot);
            StopReplay(slot);
            return false;
        }
    }
    if (!WriteLocalViewAnglesToPawn(pawn, commandView.pitch, commandView.yaw))
    {
        StopReplay(slot);
        return false;
    }
    WriteReplayViewHistory(services, pawn, commandView.pitch, commandView.yaw);
    return true;
}

// Supplies the recorded pre state to the mover without dirtying pawn transforms.
bool OnReplaySetupMove(int slot, void* moveData)
{
    if (!ValidSlot(slot) || !moveData || !IsReplaying(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (p.needsInitialTeleport.load(std::memory_order_acquire)) return false;
    MovementSnapshot pre{};
    {
        std::scoped_lock lk(p.mu);
        int cur = p.cursor.load(std::memory_order_relaxed);
        if (cur < 0 || cur >= static_cast<int>(p.frames.size())) return false;
        pre = p.frames[cur].tick.pre;
    }
    if (!WriteVector3(moveData, tg::g_moveVelocity, pre.velX, pre.velY, pre.velZ) ||
        !WriteVector3(moveData, tg::g_moveAbsOrigin, pre.originX, pre.originY, pre.originZ))
    {
        BC_LOG_WARN("Cannot supply replay movement input for slot %d\n", slot);
        StopReplay(slot);
        return false;
    }
    return true;
}

// Prepares only the recorded view; native FinishMove retains origin, velocity and stance.
bool OnReplayFinalView(int slot, void* services)
{
    if (!ValidSlot(slot) || !services || !IsReplaying(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (p.needsInitialTeleport.load(std::memory_order_acquire)) return false;
    std::scoped_lock lk(p.mu);
    const int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= static_cast<int>(p.frames.size())) return false;
    const MovementSnapshot& post = p.frames[cur].tick.post;
    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn || !WriteLocalViewAnglesToPawn(pawn, post.pitch, post.yaw))
    {
        StopReplay(slot);
        return false;
    }
    WriteReplayViewHistory(services, pawn, post.pitch, post.yaw);
    p.lastFinalViewCursor = cur;
    return true;
}

// Advances only a completed movement frame, after the engine has published its view.
void OnReplayCommit(int slot, void* services, bool simulated)
{
    if (!ValidSlot(slot) || !services || !simulated || !IsReplaying(slot)) return;
    ReplayState& p = g_rep[slot];
    std::scoped_lock lk(p.mu);
    if (p.holding.load(std::memory_order_acquire)) return;
    const int total = static_cast<int>(p.frames.size());
    const int next = p.cursor.load(std::memory_order_relaxed) + 1;
    if (p.holdBeforeCursor >= 0 && next >= p.holdBeforeCursor)
    {
        p.cursor.store(p.holdBeforeCursor, std::memory_order_relaxed);
        p.holding.store(true, std::memory_order_release);
        return;
    }
    if (next >= total && p.loop.load(std::memory_order_relaxed) && total > 0)
    {
        // Prepare the next pass immediately, without an unowned command between loops.
        p.cursor.store(p.startCursor, std::memory_order_relaxed);
        p.needsInitialTeleport.store(true, std::memory_order_release);
        p.needsHistory.store(true, std::memory_order_release);
        p.lastAppliedDef.store(-1, std::memory_order_relaxed);
        p.lastEventCursor = -1;
        p.lastFinalViewCursor = -1;
        return;
    }
    p.cursor.store(next, std::memory_order_relaxed);
    if (next >= total) StopReplay(slot);
}

void ClearAll()
{
    g_hookDropReleaseOuter.Remove();
    g_hookDropReleaseBuilder.Remove();
    g_dropHookReady.store(false, std::memory_order_release);
    g_hookDropWeapon.Remove();
    g_addrDropWeapon = nullptr;
    g_dropHookTried.store(false, std::memory_order_release);
    for (int i = 0; i < kMaxSlots; ++i)
    {
        g_rec[i].recording.store(false, std::memory_order_release);
        g_rep[i].playing.store(false, std::memory_order_release);
        g_rep[i].holding.store(false, std::memory_order_release);
        g_rep[i].needsInitialTeleport.store(false, std::memory_order_release);
        g_rep[i].needsHistory.store(false, std::memory_order_release);
        g_rep[i].loop.store(false, std::memory_order_relaxed);
        {
            std::scoped_lock lk(g_rec[i].mu);
            g_rec[i].ticks.clear();
            g_rec[i].subs.clear();
            g_rec[i].commands.clear();
            g_rec[i].pendingSubs.clear();
            g_rec[i].pendingCommand = {};
            g_rec[i].havePendingCommand = false;
            g_rec[i].havePre = false;
            g_rec[i].pendingEventFlags = ReplayEventNone;
            g_rec[i].pendingDropEvent = {};
            g_rec[i].pendingDropEvent.weaponDefIndex = -1;
            g_rec[i].pendingDropCandidates.clear();
        }
        {
            std::scoped_lock lk(g_rep[i].mu);
            g_rep[i].frames.clear();
            g_rep[i].subs.clear();
            g_rep[i].subOffset.clear();
            g_rep[i].startCursor = 0;
            g_rep[i].holdBeforeCursor = -1;
            g_rep[i].lastEventCursor = -1;
            g_rep[i].lastFinalViewCursor = -1;
        }
        g_rec[i].currentDef.store(-1, std::memory_order_relaxed);
        g_rec[i].liveWs.store(nullptr, std::memory_order_relaxed);
        g_rep[i].cursor.store(0, std::memory_order_relaxed);
        g_rep[i].lastAppliedDef.store(-1, std::memory_order_relaxed);
        input_injector::ClearReplayPawn(i);
    }
    g_recordingSlots.store(0, std::memory_order_release);
    g_replayingSlots.store(0, std::memory_order_release);
}
} // namespace motion_recorder
} // namespace cs2bc
