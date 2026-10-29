// Motion recording & replay implementation

#include "MotionRecorder.h"
#include "CommandHistory.h"
#include "BotController.h"
#include "dispatch.h"
#include "InputInjector.h"
#include "PawnBinding.h"
#include "WeaponLocker.h"
#include "ccsbot_slot.h"
#include "core/cs2_sdk/src/utils/hooks.h"
#include "core/log.h"
#include "offsets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <mutex>
#include <vector> // NOLINT(misc-include-cleaner)

#include <convar.h>
#include <const.h>
#include <engine2/isource2server.h>
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
    std::vector<uint8_t> inputHistory;
    // Input buffers awaiting the matching PlayerRunCommand post.
    std::vector<SubtickMove> pendingSubs;
    ReplayCommandFrameData pendingCommand{};
    std::string pendingHistory;
    bool havePendingCommand{ false };
    MovementSnapshot pendingPre{};
    bool havePre{ false };
    uint32_t pendingEventFlags{ ReplayEventNone };
    ReplayDropEvent pendingDropEvent{ .weaponDefIndex = -1, .vectorFlags = ReplayDropVectorNone, .target = {}, .velocity = {} };
    std::vector<DropCandidate> pendingDropCandidates;
    std::atomic<void*> liveWs{ nullptr };
    std::atomic<int> currentDef{ -1 };
    std::mutex mu; // guards ticks/subs/pending/pre

    // Resets the command; retains pending drops.
    void ResetCommand()
    {
        pendingSubs.clear();
        pendingCommand = {};
        pendingHistory.clear();
        havePendingCommand = false;
        havePre = false;
        pendingEventFlags = ReplayEventNone;
        pendingDropEvent = {};
        pendingDropEvent.weaponDefIndex = -1;
    }

    // Clears pending session capture.
    void ResetPending()
    {
        ResetCommand();
        pendingDropCandidates.clear();
    }
};

struct ReplayState
{
    std::atomic<bool> playing{ false };
    std::atomic<bool> needsInitialTeleport{ false };
    std::atomic<bool> loop{ false };
    std::vector<ReplayTick> ticks;
    std::vector<SubtickMove> subs;
    std::vector<ReplayCommandFrameData> commands;
    std::vector<CSGOUserCmdPB> inputHistory;
    std::vector<uint32_t> subOffset; // prefix sum, size ticks.size()+1
    std::atomic<int> cursor{ 0 };
    std::atomic<uint64_t> generation{ 1 };
    int lastEventCursor{ -1 };
    int lastCompletedCursor{ -1 };
    std::mutex mu; // guards ticks/subs/subOffset
};

namespace {

std::array<RecordState, kMaxSlots> g_rec;
std::array<ReplayState, kMaxSlots> g_rep;
std::atomic<uint64_t> g_recordingSlots{ 0 };
std::atomic<uint64_t> g_replayingSlots{ 0 };
static_assert(kMaxSlots <= 64);
cs2_sdk::hooks::NativeHook<void, void*, void*, void*, const float*> g_hookDropWeapon;
struct WeaponSession
{
    int slot{ -1 };
    uint64_t generation{ 0 };
    void* movementServices{ nullptr };
    void* weaponServices{ nullptr };
};
struct DropFrame
{
    WeaponSession session;
    void* weapon;
    int weaponDefIndex;
    ReplayDropEvent recordedEvent;
};
thread_local std::vector<DropFrame> g_dropFrames;
void* g_addrDropWeapon = nullptr;
std::atomic<bool> g_dropHookTried{ false };
std::atomic<bool> g_dropHookReady{ false };
cs2_sdk::hooks::NativeHook<char, void*, void*, int, const float*> g_hookDropReleaseOuter;
cs2_sdk::hooks::NativeHook<char, void*, void*> g_hookDropReleaseBuilder;
struct DropReleaseFrame
{
    WeaponSession session;
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
    WeaponSession session;
    void* weapon;
    ReplayDropEvent event;
};
thread_local std::vector<ReplayDropOverride> g_replayDropEvents;
constexpr int kMolotovDef = 46;
constexpr int kIncendiaryDef = 48;

bool ValidSlot(int s) { return s >= 0 && s < kMaxSlots; }

bool Finite(std::initializer_list<float> values)
{
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

// Validates replay snapshots.
bool ValidSnapshot(const MovementSnapshot& s)
{
    return Finite({ s.originX, s.originY, s.originZ, s.velX, s.velY, s.velZ, s.pitch, s.yaw, s.roll, s.duckAmount, s.duckSpeed,
                    s.ladderNormalX, s.ladderNormalY, s.ladderNormalZ }) &&
           s.duckAmount >= 0.0F && s.duckAmount <= 1.0F && s.moveType < MOVETYPE_LAST && s.actualMoveType < MOVETYPE_LAST &&
           s.ducked <= 1 && s.ducking <= 1 && s.desiresDuck <= 1;
}

bool ValidCommand(const ReplayCommandFrameData& command)
{
    constexpr uint32_t fields = kCommandFieldForwardMove | kCommandFieldLeftMove | kCommandFieldUpMove | kCommandFieldViewAngles |
                                kCommandFieldButtons | kCommandFieldMouse | kCommandFieldWeaponSelect | kCommandFieldLeftHand |
                                kCommandFieldWeaponSelectDef;
    return (command.fields & ~fields) == 0 && ((command.fields & kCommandFieldForwardMove) == 0 || std::isfinite(command.forwardMove)) &&
           ((command.fields & kCommandFieldLeftMove) == 0 || std::isfinite(command.leftMove)) &&
           ((command.fields & kCommandFieldUpMove) == 0 || std::isfinite(command.upMove)) &&
           ((command.fields & kCommandFieldViewAngles) == 0 || Finite({ command.pitch, command.yaw, command.roll })) &&
           ((command.fields & kCommandFieldLeftHand) == 0 || command.leftHandDesired <= 1) &&
           ((command.fields & kCommandFieldWeaponSelect) == 0 || (command.weaponSelect >= -1 && command.weaponSelect <= UINT16_MAX));
}

// Validates the drop release transform.
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
static int ReplayWeaponSelectForDef(const WeaponSession& session, int recordedDef);
// Restores Pawn state before native drops.
bool PrepareReplayDropPawn(int slot, void* services, const ReplayDropEvent& event);
void* ResolveSceneNode(void* entity);

// Checks either faction's fire grenade.
bool IsFireGrenadeDef(int defIndex) { return defIndex == kMolotovDef || defIndex == kIncendiaryDef; }

// Prefers the recorded fire grenade
void* FindReplayWeaponByDef(void* ws, int recordedDef)
{
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, recordedDef);
    if (weapon || !IsFireGrenadeDef(recordedDef)) return weapon;

    const int alternateDef = recordedDef == kMolotovDef ? kIncendiaryDef : kMolotovDef;
    return weapon_locker_hooks::FindWeaponByDef(ws, alternateDef);
}

// Revalidates drop ownership.
bool WeaponSessionCurrent(const WeaponSession& session)
{
    if (!ValidSlot(session.slot) || !session.weaponServices || session.generation != PawnGeneration(session.slot)) return false;
    void* weapons = input_injector::pawn_binding::ServicesToWeaponServices(session.slot, session.movementServices);
    return weapons == session.weaponServices && session.generation == PawnGeneration(session.slot);
}

// Resolves the registered capture session.
WeaponSession CurrentWeaponSession(void* weaponServices)
{
    void* pawn = nullptr;
    if (!weaponServices || !GuardedRead(weaponServices, tg::g_servicesPawn, pawn)) return {};
    const int slot = ControllerSlotForPawn(pawn);
    if (!IsRecording(slot) && !IsReplaying(slot)) return {};
    const uint64_t generation = PawnGeneration(slot);
    WeaponSession session{ slot, generation, input_injector::pawn_binding::ServicesForSlot(slot), weaponServices };
    return WeaponSessionCurrent(session) ? session : WeaponSession{};
}

// Records the drop event and item
KHook::Return<void> HookedDropWeapon(void* weaponServices, void* weapon, void*, const float*) noexcept
{
    const WeaponSession session = CurrentWeaponSession(weaponServices);
    const int recordingSlot = session.slot;
    const int weaponDefIndex = weapon_locker_hooks::ReadDefIndex(weapon);

    ReplayDropEvent recordedEvent{};
    recordedEvent.weaponDefIndex = -1;
    // Keep raw definitions for detach checks; normalize recorded knives.
    if (IsRecording(recordingSlot))
    {
        recordedEvent.weaponDefIndex =
            weapon_locker_hooks::WeaponDefForEntityIndex(weaponServices, weapon_locker_hooks::WeaponEntIndex(weapon));
        if (recordedEvent.weaponDefIndex < 0) recordedEvent.weaponDefIndex = weaponDefIndex;
        if (!WeaponSessionCurrent(session) || !IsRecording(recordingSlot))
        {
            recordedEvent.weaponDefIndex = -1;
            g_dropFrames.push_back({ session, weapon, weaponDefIndex, recordedEvent });
            return { KHook::Action::Ignore };
        }
        void* node = ResolveSceneNode(input_injector::ResolveReplayPawn(recordingSlot, session.movementServices));
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
            if (frame.session.slot == session.slot && frame.session.generation == session.generation &&
                frame.droppedWeapon == weapon && frame.haveReleasePose)
            {
                recordedEvent.vectorFlags |= ReplayDropReleasePose;
                std::copy_n(frame.releasePosition, 3, recordedEvent.releasePosition);
                std::copy_n(frame.releaseQuaternion, 4, recordedEvent.releaseQuaternion);
            }
        }
    }
    g_dropFrames.push_back({ session, weapon, weaponDefIndex, recordedEvent });
    return { KHook::Action::Ignore };
}

// Commits detachments to the original Pawn session.
KHook::Return<void> DropWeaponPost(void*, void*, void*, const float*) noexcept
{
    const auto [session, weapon, weaponDefIndex, recordedEvent] = g_dropFrames.back();
    g_dropFrames.pop_back();
    if (recordedEvent.weaponDefIndex < 0 || !IsRecording(session.slot) || !WeaponSessionCurrent(session))
        return { KHook::Action::Ignore };
    const bool detached =
        weapon && weaponDefIndex >= 0 && weapon_locker_hooks::FindWeaponByDef(session.weaponServices, weaponDefIndex) != weapon;
    if (!WeaponSessionCurrent(session)) return { KHook::Action::Ignore };
    RecordState& r = g_rec[session.slot];
    std::scoped_lock lk(r.mu);
    if (!r.recording.load(std::memory_order_acquire) || session.generation != PawnGeneration(session.slot))
        return { KHook::Action::Ignore };
    if (detached)
    {
        r.pendingEventFlags |= ReplayEventDrop;
        r.pendingDropEvent = recordedEvent;
    }
    else if (weapon && weaponDefIndex >= 0)
    {
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

// Hooks drops from the live weapon-services vtable.
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

// Reads an engine vector with one guarded copy.
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

// Tracks stacked-grenade splits during drops.
KHook::Return<char> DropReleaseOuterPre(void* weaponServices, void* weapon, int, const float*) noexcept
{
    DropReleaseFrame frame{};
    frame.session = CurrentWeaponSession(weaponServices);
    frame.weapon = weapon;
    frame.defIndex = weapon_locker_hooks::ReadDefIndex(weapon);
    g_dropReleaseFrames.push_back(frame);
    return { KHook::Action::Ignore };
}

// Pops the drop frame after the native call.
KHook::Return<char> DropReleaseOuterPost(void*, void*, int, const float*) noexcept
{
    if (!g_dropReleaseFrames.empty()) g_dropReleaseFrames.pop_back();
    return { KHook::Action::Ignore };
}

// Binds release transforms to the dropped item.
KHook::Return<char> DropReleaseBuilderPre(void* weapon, void*) noexcept
{
    if (!g_dropReleaseFrames.empty())
    {
        DropReleaseFrame& frame = g_dropReleaseFrames.back();
        if (WeaponSessionCurrent(frame.session) && weapon_locker_hooks::ReadDefIndex(weapon) == frame.defIndex) frame.droppedWeapon = weapon;
    }
    return { KHook::Action::Ignore };
}

// Captures or restores the drop release pose.
KHook::Return<char> DropReleaseBuilderPost(void* weapon, void* transform) noexcept
{
    if (g_dropReleaseFrames.empty()) return { KHook::Action::Ignore };
    DropReleaseFrame& frame = g_dropReleaseFrames.back();
    if (frame.droppedWeapon != weapon || !WeaponSessionCurrent(frame.session)) return { KHook::Action::Ignore };
    float output[8]{};
    const bool readable = transform && TryReadMemoryGuarded(transform, 0, output, sizeof(output));
    if (readable && IsRecording(frame.session.slot) && ValidReleasePose(output, output + 4))
    {
        std::copy_n(output, 3, frame.releasePosition);
        std::copy_n(output + 4, 4, frame.releaseQuaternion);
        frame.haveReleasePose = true;
    }
    else if (readable && IsReplaying(frame.session.slot) && !g_replayDropEvents.empty())
    {
        const ReplayDropOverride& override = g_replayDropEvents.back();
        const ReplayDropEvent& event = override.event;
        if (override.session.slot == frame.session.slot && override.session.generation == frame.session.generation &&
            override.weapon == frame.weapon && (event.vectorFlags & ReplayDropReleasePose) != 0)
        {
            std::copy_n(event.releasePosition, 3, output);
            std::copy_n(event.releaseQuaternion, 4, output + 4);
            if (!TryWriteMemoryGuarded(transform, 0, output, sizeof(output)))
                BC_LOG_WARN("slot %d: drop pose failed", frame.session.slot);
        }
    }
    return { KHook::Action::Ignore };
}

// Reads a snapshot from the registered Pawn.
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

// Installs drop release-pose hooks.
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
    if (!ValidSlot(slot) || IsReplaying(slot) || !input_injector::RecorderReady() ||
        !input_injector::pawn_binding::ServicesForSlot(slot))
        return false;
    RecordState& r = g_rec[slot];
    {
        std::scoped_lock lk(r.mu, g_rep[slot].mu);
        g_rep[slot].generation.fetch_add(1, std::memory_order_release);
        r.ticks.clear();
        r.subs.clear();
        r.commands.clear();
        r.inputHistory.clear();
        r.ResetPending();
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
    if (g_rec[slot].recording.load(std::memory_order_acquire)) InvalidateSlot(slot);
    return true;
}

// Ends the session and retains recorded data.
void InvalidateSlot(int slot)
{
    if (!ValidSlot(slot)) return;
    RecordState& r = g_rec[slot];
    ReplayState& p = g_rep[slot];
    {
        std::scoped_lock lock(r.mu, p.mu);
        r.recording.store(false, std::memory_order_release);
        p.playing.store(false, std::memory_order_release);
        p.generation.fetch_add(1, std::memory_order_release);
        g_recordingSlots.fetch_and(~(uint64_t{ 1 } << slot), std::memory_order_release);
        g_replayingSlots.fetch_and(~(uint64_t{ 1 } << slot), std::memory_order_release);
        r.ResetPending();
        r.liveWs.store(nullptr, std::memory_order_relaxed);
        r.currentDef.store(-1, std::memory_order_relaxed);
        p.lastEventCursor = -1;
    }
    input_injector::ClearReplayPawn(slot);
    input_injector::ClearUsercmdInjections(slot);
}

// Session generation 0 is invalid.
uint64_t PawnGeneration(int slot)
{
    return ValidSlot(slot) ? g_rep[slot].generation.load(std::memory_order_acquire) : 0;
}

bool IsRecording(int slot) { return ValidSlot(slot) && g_rec[slot].recording.load(std::memory_order_acquire); }

// Checks for active recordings.
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

// Counts complete recorded commands.
int RecordedCommandCount(int slot)
{
    if (!ValidSlot(slot)) return -1;
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    return static_cast<int>(r.commands.size());
}

// Recorded history size in bytes.
int RecordedInputHistorySize(int slot)
{
    if (!ValidSlot(slot)) return -1;
    RecordState& r = g_rec[slot];
    std::scoped_lock lock(r.mu);
    return static_cast<int>(r.inputHistory.size());
}

void SetLiveWs(int slot, void* ws)
{
    if (!ValidSlot(slot)) return;
    g_rec[slot].liveWs.store(ws, std::memory_order_relaxed);
    EnsureDropWeaponHook(ws);
}

void SetCurrentDef(int slot, int defIndex)
{
    if (ValidSlot(slot)) g_rec[slot].currentDef.store(defIndex, std::memory_order_relaxed);
}

// Captures state before queued drops and movement.
bool OnRecordCommandPre(int slot, void* services)
{
    if (!ValidSlot(slot) || !services) return false;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return false;
    const uint64_t generation = PawnGeneration(slot);
    {
        std::scoped_lock lock(r.mu);
        if (!r.recording.load(std::memory_order_acquire) || generation != PawnGeneration(slot)) return false;
        r.ResetCommand();
    }
    SetLiveWs(slot, input_injector::pawn_binding::ServicesToWeaponServices(slot, services));
    MovementSnapshot pre{};
    if (!ReadSnapshot(slot, services, pre)) return false;
    std::scoped_lock lk(r.mu);
    if (!r.recording.load(std::memory_order_acquire) || generation != PawnGeneration(slot)) return false;
    r.pendingPre = pre;
    r.havePre = true;
    return true;
}

void OnCaptureSubticks(int slot, const SubtickMove* moves, int count)
{
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    if (!r.recording.load(std::memory_order_acquire)) return;
    r.pendingSubs.clear();
    for (int i = 0; i < count; ++i)
        r.pendingSubs.push_back(moves[i]);
}

// Holds input until command completion.
void OnCaptureCommand(int slot, const ReplayCommandFrameData& command, std::string history)
{
    RecordState& r = g_rec[slot];
    std::scoped_lock lk(r.mu);
    if (!r.recording.load(std::memory_order_acquire)) return;
    r.pendingCommand = command;
    r.pendingHistory = std::move(history);
    r.havePendingCommand = true;
}

// Commits one command; engine calls run unlocked.
void OnRecordCommandPost(int slot, void* services)
{
    if (!ValidSlot(slot) || !services) return;
    RecordState& r = g_rec[slot];
    if (!r.recording.load(std::memory_order_acquire)) return;
    const uint64_t generation = PawnGeneration(slot);
    {
        std::scoped_lock lock(r.mu);
        if (!r.havePre || !r.havePendingCommand) return;
    }

    MovementSnapshot post{};
    if (!ReadSnapshot(slot, services, post)) return;

    // Read inventory after this command's native weapon processing.
    void* ws = r.liveWs.load(std::memory_order_relaxed);
    int def = weapon_locker_hooks::ActiveWeaponDef(ws);
    if (def < 0) def = r.currentDef.load(std::memory_order_relaxed);
    std::vector<RecordState::DropCandidate> dropped;
    {
        std::scoped_lock lk(r.mu);
        dropped = r.pendingDropCandidates;
    }
    std::erase_if(dropped, [ws](const auto& candidate) {
        return weapon_locker_hooks::FindWeaponByDef(ws, candidate.event.weaponDefIndex) == candidate.weapon;
    });

    {
        std::scoped_lock lk(r.mu);
        if (!r.recording.load(std::memory_order_acquire) || generation != PawnGeneration(slot) || !r.havePre || !r.havePendingCommand)
            return;
        if (def >= 0) r.currentDef.store(def, std::memory_order_relaxed);
        for (const auto& candidate : dropped)
        {
            r.pendingEventFlags |= ReplayEventDrop;
            r.pendingDropEvent = candidate.event;
            std::erase_if(r.pendingDropCandidates, [&](const auto& pending) { return pending.weapon == candidate.weapon; });
        }

        ReplayTick t{};
        t.pre = r.pendingPre;
        t.post = post;
        t.weaponDefIndex = def;
        t.numSubtick = static_cast<uint32_t>(r.pendingSubs.size());
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
        r.commands.push_back(r.pendingCommand);
        // Keep history separate from the ABI 24 command layout.
        const auto historySize = static_cast<uint32_t>(r.pendingHistory.size());
        for (unsigned shift = 0; shift < 32; shift += 8)
            r.inputHistory.push_back(static_cast<uint8_t>(historySize >> shift));
        r.inputHistory.insert(r.inputHistory.end(), r.pendingHistory.begin(), r.pendingHistory.end());
        r.ResetCommand();
    }
}

int CopyTicks(int slot, ReplayTick* out, int maxTicks) { return CopyTicksRange(slot, 0, out, maxTicks); }

// Copies a bounded tick range.
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

// Copies a bounded recorded subtick range.
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

// Copies a bounded recorded command range.
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

// Copies serialized input history.
int CopyInputHistoryRange(int slot, int start, uint8_t* out, int maxBytes)
{
    if (!ValidSlot(slot) || start < 0 || !out || maxBytes <= 0) return 0;
    RecordState& r = g_rec[slot];
    std::scoped_lock lock(r.mu);
    const int count = std::min(maxBytes, std::max(0, static_cast<int>(r.inputHistory.size()) - start));
    if (count > 0) std::copy_n(r.inputHistory.data() + start, count, out);
    return count;
}

// ---- replay ----

// Validate, stage, and atomically replace all replay buffers
bool LoadReplay(int slot,
                const ReplayTick* ticks,
                int tickCount,
                const SubtickMove* subs,
                int subCount,
                const ReplayCommandFrameData* commands,
                int commandCount,
                const uint8_t* inputHistory,
                int inputHistorySize,
                uint64_t pawnGeneration) noexcept
{
    try
    {
        if (!ValidSlot(slot) || !ticks || tickCount < 0 || subCount < 0 || (subCount > 0 && !subs) ||
            (commandCount != 0 && commandCount != tickCount) || (commandCount > 0 && !commands) ||
            inputHistorySize < 0 || (inputHistorySize > 0 && !inputHistory))
        {
            return false;
        }

        // Each frame has a length prefix, even when empty.
        if (inputHistorySize > 0 && static_cast<size_t>(tickCount) > static_cast<size_t>(inputHistorySize) / sizeof(uint32_t))
            return false;

        ReplayState& p = g_rep[slot];
        const uint64_t expectedGeneration = pawnGeneration ? pawnGeneration : PawnGeneration(slot);
        if (expectedGeneration != PawnGeneration(slot)) return false;
        if (p.playing.load(std::memory_order_acquire) || IsRecording(slot)) return false;

        std::vector<ReplayTick> stagedTicks;
        std::vector<SubtickMove> stagedSubs;
        std::vector<ReplayCommandFrameData> stagedCommands;
        std::vector<CSGOUserCmdPB> stagedHistory;
        const auto subCountValue = static_cast<uint64_t>(subCount);
        std::vector<uint32_t> stagedOffsets(static_cast<size_t>(tickCount) + 1, 0);

        uint64_t totalSubticks = 0;
        for (int i = 0; i < tickCount; ++i)
        {
            if (!ValidSnapshot(ticks[i].pre) || !ValidSnapshot(ticks[i].post) || ticks[i].weaponDefIndex < -1 ||
                ticks[i].weaponDefIndex > UINT16_MAX || ticks[i].numSubtick > kMaxSubtickPerTick ||
                (ticks[i].eventFlags & ~static_cast<uint32_t>(ReplayEventDrop)) != 0)
                return false;
            if ((ticks[i].eventFlags & ReplayEventDrop) != 0)
            {
                constexpr uint32_t vectorFlags =
                    ReplayDropVectorTarget | ReplayDropVectorVelocity | ReplayDropBodyYaw | ReplayDropReleasePose;
                const uint32_t flags = ticks[i].eventDropVectorFlags;
                if (ticks[i].eventWeaponDefIndex < 0 || ticks[i].eventWeaponDefIndex > UINT16_MAX || (flags & ~vectorFlags) != 0)
                    return false;
                if ((flags & ReplayDropVectorTarget) != 0 &&
                    !Finite({ ticks[i].eventDropTargetX, ticks[i].eventDropTargetY, ticks[i].eventDropTargetZ }))
                    return false;
                if ((flags & ReplayDropBodyYaw) != 0 && !std::isfinite(ticks[i].eventDropTargetX)) return false;
                if ((flags & ReplayDropVectorVelocity) != 0 &&
                    !Finite({ ticks[i].eventDropVelocityX, ticks[i].eventDropVelocityY, ticks[i].eventDropVelocityZ }))
                    return false;
                const float position[3] = { ticks[i].eventDropReleaseX, ticks[i].eventDropReleaseY, ticks[i].eventDropReleaseZ };
                const float quaternion[4] = { ticks[i].eventDropReleaseQuatX, ticks[i].eventDropReleaseQuatY,
                                              ticks[i].eventDropReleaseQuatZ, ticks[i].eventDropReleaseQuatW };
                if ((ticks[i].eventDropVectorFlags & ReplayDropReleasePose) == 0 || !ValidReleasePose(position, quaternion)) return false;
            }
            stagedOffsets[static_cast<size_t>(i)] = static_cast<uint32_t>(totalSubticks);
            totalSubticks += ticks[i].numSubtick;
            if (totalSubticks > subCountValue) return false;
        }
        if (totalSubticks != subCountValue) return false;
        stagedOffsets[static_cast<size_t>(tickCount)] = static_cast<uint32_t>(totalSubticks);

        for (int i = 0; i < subCount; ++i)
        {
            const SubtickMove& s = subs[i];
            if (!Finite({ s.when, s.analogForward, s.analogLeft, s.pitchDelta, s.yawDelta }) || s.when < 0.0F || s.when >= 1.0F ||
                (s.button != 0 && s.pressed != 0.0F && s.pressed != 1.0F))
                return false;
        }
        for (int i = 0; i < commandCount; ++i)
            if (!ValidCommand(commands[i])) return false;

        if (tickCount > 0) stagedTicks.assign(ticks, ticks + tickCount);
        if (subCount > 0) stagedSubs.assign(subs, subs + subCount);
        if (commandCount > 0) stagedCommands.assign(commands, commands + commandCount);
        if (inputHistorySize > 0) stagedHistory.resize(static_cast<size_t>(tickCount));
        size_t historyOffset = 0;
        for (auto& history : stagedHistory)
        {
            if (static_cast<size_t>(inputHistorySize) - historyOffset < sizeof(uint32_t)) return false;
            uint32_t size = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
                size |= static_cast<uint32_t>(inputHistory[historyOffset++]) << shift;
            if (size > static_cast<size_t>(inputHistorySize) - historyOffset) return false;
            if (size > 0 && !command_history::Parse(
                    { reinterpret_cast<const char*>(inputHistory + historyOffset), size }, history))
                return false;
            historyOffset += size;
        }
        if (historyOffset != static_cast<size_t>(inputHistorySize)) return false;

        std::scoped_lock lk(p.mu);
        if (p.playing.load(std::memory_order_acquire) || IsRecording(slot) || expectedGeneration != PawnGeneration(slot)) return false;

        p.ticks.swap(stagedTicks);
        p.subs.swap(stagedSubs);
        p.commands.swap(stagedCommands);
        p.inputHistory.swap(stagedHistory);
        p.subOffset.swap(stagedOffsets);
        p.cursor.store(0, std::memory_order_relaxed);
        p.lastEventCursor = -1;
        p.lastCompletedCursor = -1;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool StartReplay(int slot, bool loop)
{
    if (!ValidSlot(slot) || IsRecording(slot) || !input_injector::RecorderReady() ||
        !input_injector::pawn_binding::ServicesForSlot(slot))
        return false;
    if (!bot_controller_hooks::ReplayViewReady())
    {
        BC_LOG_WARN("replay: view unavailable");
        return false;
    }
    ReplayState& p = g_rep[slot];
    {
        std::scoped_lock lk(p.mu);
        if (p.ticks.empty()) return false;
        p.generation.fetch_add(1, std::memory_order_release);
        p.cursor.store(0, std::memory_order_relaxed);
        p.lastEventCursor = -1;
        p.lastCompletedCursor = -1;
        p.loop.store(loop, std::memory_order_relaxed);
        p.needsInitialTeleport.store(true, std::memory_order_release);
        g_replayingSlots.fetch_or(uint64_t{ 1 } << slot, std::memory_order_release);
        p.playing.store(true, std::memory_order_release);
    }
    input_injector::ClearUsercmdInjections(slot);
    return true;
}

bool StopReplay(int slot)
{
    if (!ValidSlot(slot) || IsRecording(slot)) return false;
    InvalidateSlot(slot);
    return true;
}

bool IsReplaying(int slot) { return ValidSlot(slot) && g_rep[slot].playing.load(std::memory_order_acquire); }

// Checks for active replays.
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
    return static_cast<int>(p.ticks.size());
}

// Keeps completed-frame queries stable across loops.
bool CurrentReplayTick(int slot, ReplayTick& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int idx = std::max(p.lastCompletedCursor, 0);
    if (idx >= total) return false;
    out = p.ticks[idx];
    return true;
}

// Assembles the next replay command.
bool ReplayCommandFrameForSimulation(int slot, void* services, ReplayCommandFrame& out)
{
    out = {};
    out.weaponSelect = -1;
    out.rawWeaponSelect = -1;
    if (!ValidSlot(slot) || !services) return false;

    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    const uint64_t generation = PawnGeneration(slot);

    int recordedDef = -1;
    bool recordedWeaponChanged = false;
    {
        std::scoped_lock lk(p.mu);
        const int total = static_cast<int>(p.ticks.size());
        const int cur = p.cursor.load(std::memory_order_relaxed);
        if (cur < 0 || cur >= total) return false;

        out.tick = p.ticks[static_cast<size_t>(cur)];
        recordedDef = out.tick.weaponDefIndex;
        recordedWeaponChanged = cur > 0 && !ReplayWeaponDefsMatch(p.ticks[static_cast<size_t>(cur - 1)].weaponDefIndex, recordedDef);
        out.commandView = out.tick.pre;
        out.buttons0 = out.tick.pre.buttons;
        out.buttons1 = out.tick.pre.buttons1;
        out.buttons2 = out.tick.pre.buttons2;

        const ReplayCommandFrameData* command = p.commands.empty() ? nullptr : &p.commands[static_cast<size_t>(cur)];
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
            out.buttons1 = out.buttons0 ^ heldPrev;
            out.buttons2 = 0;
        }

        if (!p.inputHistory.empty()) out.inputHistory.CopyFrom(p.inputHistory[static_cast<size_t>(cur)]);
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
        out.subtickCount = static_cast<int32_t>(end - begin);
        for (int i = 0; i < out.subtickCount; ++i)
            out.subticks[i] = p.subs[static_cast<size_t>(begin) + static_cast<size_t>(i)];
    }

    void* ws = input_injector::pawn_binding::ServicesToWeaponServices(slot, services);
    const WeaponSession session{ slot, generation, services, ws };
    if (!ws || generation != PawnGeneration(slot)) return false;

    // Resolve the registered replay inventory.
    if (out.rawWeaponSelect > 0)
    {
        void* weapon = FindReplayWeaponByDef(ws, out.rawWeaponSelect);
        if (!WeaponSessionCurrent(session)) return false;
        if (weapon) out.weaponSelect = weapon_locker_hooks::WeaponEntIndex(weapon);
    }
    else if (p.needsInitialTeleport.load(std::memory_order_acquire) || recordedWeaponChanged)
    {
        // Preserve client-command weapon selections.
        out.weaponSelect = ReplayWeaponSelectForDef(session, recordedDef);
    }
    return IsReplaying(slot) && WeaponSessionCurrent(session);
}

bool ReplayCommandViewSnapshot(int slot, MovementSnapshot& out)
{
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;
    std::scoped_lock lk(p.mu);
    int total = static_cast<int>(p.ticks.size());
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || cur >= total) return false;
    out = p.ticks[cur].pre;
    return true;
}

// Switches a live Pawn through selection hooks.
bool SwitchWeaponByDef(int slot, int defIndex)
{
    if (!ValidSlot(slot) || defIndex < 0 || IsReplaying(slot)) return false;
    if (!weapon_locker_hooks::WeaponHooksReady()) return false;
    void* ws = input_injector::pawn_binding::CurrentWeaponServices(slot);
    if (!ws) return false;
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, defIndex);
    if (!weapon) return false;
    if (IsReplaying(slot) || input_injector::pawn_binding::CurrentWeaponServices(slot) != ws) return false;
    return weapon_locker_hooks::SelectWeapon(ws, weapon);
}

// Queries registered sessions or the current live Pawn.
int ActiveWeaponDefForSlot(int slot)
{
    if (!ValidSlot(slot) || !weapon_locker_hooks::WeaponHooksReady()) return -1;
    const bool activeSession = IsRecording(slot) || IsReplaying(slot);
    void* ws = activeSession ? input_injector::pawn_binding::RegisteredWeaponServices(slot)
                            : input_injector::pawn_binding::CurrentWeaponServices(slot);
    if (!ws) return -1;
    return weapon_locker_hooks::ActiveWeaponDef(ws);
}

// Matches CT and T fire grenades.
bool ReplayWeaponDefsMatch(int firstDef, int secondDef)
{
    return firstDef == secondDef || (IsFireGrenadeDef(firstDef) && IsFireGrenadeDef(secondDef));
}

namespace {

// Selects the recorded weapon as needed.
int ReplayWeaponSelectForDef(const WeaponSession& session, int recordedDef)
{
    if (!weapon_locker_hooks::WeaponHooksReady() || !WeaponSessionCurrent(session)) return -1;
    if (recordedDef < 0) return -1;

    void* ws = session.weaponServices;

    // Skip the active weapon.
    const int activeDef = weapon_locker_hooks::ActiveWeaponDef(ws);
    if (!WeaponSessionCurrent(session) || ReplayWeaponDefsMatch(activeDef, recordedDef)) return -1;

    void* weapon = FindReplayWeaponByDef(ws, recordedDef);
    if (!weapon || !WeaponSessionCurrent(session)) return -1;
    if (!weapon_locker_hooks::SelectWeaponRaw(ws, weapon) || !WeaponSessionCurrent(session)) return -1;
    return weapon_locker_hooks::WeaponEntIndex(weapon);
}
} // namespace

// Consumes a drop event once per replay cursor.
bool TakeCurrentReplayDrop(int slot, ReplayDropEvent& event)
{
    event = {};
    event.weaponDefIndex = -1;
    if (!ValidSlot(slot)) return false;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return false;

    std::scoped_lock lk(p.mu);
    int cur = p.cursor.load(std::memory_order_relaxed);
    if (cur < 0 || static_cast<size_t>(cur) >= p.ticks.size() || p.lastEventCursor == cur) return false;
    p.lastEventCursor = cur;

    const ReplayTick& tick = p.ticks[cur];
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

// Drops through the native client-command path.
bool DropReplayEventWeapon(int slot, void* services, const ReplayDropEvent& event)
{
    const int weaponDefIndex = event.weaponDefIndex;
    if (!IsReplaying(slot) || !weapon_locker_hooks::WeaponHooksReady() || !g_hookDropReleaseBuilder.Active()) return false;

    const uint64_t generation = PawnGeneration(slot);
    void* ws = input_injector::pawn_binding::ServicesToWeaponServices(slot, services);
    const WeaponSession session{ slot, generation, services, ws };
    if (!ws || generation != PawnGeneration(slot)) return false;
    void* weapon = weapon_locker_hooks::FindWeaponByDef(ws, weaponDefIndex);
    if (!weapon || !WeaponSessionCurrent(session)) return false;
    const int replayDef = weapon_locker_hooks::WeaponDefForEntityIndex(ws, weapon_locker_hooks::WeaponEntIndex(weapon));
    if (replayDef != weaponDefIndex || !WeaponSessionCurrent(session)) return false;
    const int activeDef = weapon_locker_hooks::ActiveWeaponDef(ws);
    if (!WeaponSessionCurrent(session)) return false;
    if (activeDef != weaponDefIndex && !weapon_locker_hooks::SelectWeaponRaw(ws, weapon)) return false;
    if (!WeaponSessionCurrent(session)) return false;
    if (weapon_locker_hooks::ActiveWeaponDef(ws) != weaponDefIndex || !WeaponSessionCurrent(session)) return false;

    if (!dispatch::g_gameClients) return false;
    CCommand command;
    if (!command.Tokenize("drop")) return false;

    if (!PrepareReplayDropPawn(slot, services, event) || !WeaponSessionCurrent(session)) return false;
    g_replayDropEvents.push_back({ session, weapon, event });
    dispatch::g_gameClients->ClientCommand(CPlayerSlot(slot), command);
    g_replayDropEvents.pop_back();
    return true;
}

// Checks the native drop hook.
bool DropHookReady() { return g_dropHookReady.load(std::memory_order_acquire); }

// Restores replay velocity.
namespace {

// Restores the registered Pawn's velocity.
void WriteVelocityToPawn(void* pawn, const MovementSnapshot& s)
{
    if (!pawn) return;
    WriteVector3(pawn, tg::g_entAbsVelocity, s.velX, s.velY, s.velZ);
}

// Restores scene-node origin.
void WriteSceneNodeOrigin(void* pawn, const MovementSnapshot& s)
{
    if (!pawn) return;

    void* node = ResolveSceneNode(pawn);
    if (!node) return;

    const float values[3] = { s.originX, s.originY, s.originZ };
    TryWriteMemoryGuarded(node, tg::g_nodeAbsOrigin, values, sizeof(values));
}

// Restores duck and ladder state.
void WriteMovementServiceState(void* services, const MovementSnapshot& s)
{
    WriteField(services, tg::g_servicesDuckAmount, s.duckAmount);
    WriteField(services, tg::g_servicesDuckSpeed, s.duckSpeed);
    WriteVector3(services, tg::g_servicesLadderNormal, s.ladderNormalX, s.ladderNormalY, s.ladderNormalZ);
    WriteField(services, tg::g_servicesDucked, s.ducked);
    WriteField(services, tg::g_servicesDucking, s.ducking);
    WriteField(services, tg::g_servicesDesiresDuck, s.desiresDuck);
}

// Normalizes replay yaw.
float NormalizeReplayYaw(float yaw)
{
    yaw = std::fmod(yaw + 180.0F, 360.0F);
    if (yaw < 0.0F) yaw += 360.0F;
    return yaw - 180.0F;
}

// Restores entry angles for grenade parameter caching.
void WriteRawViewAnglesToPawn(void* pawn, float pitch, float yaw)
{
    const float normalizedYaw = NormalizeReplayYaw(yaw);
    WriteVector3(pawn, tg::g_pawnViewAngle, pitch, normalizedYaw, 0.0F);
    WriteVector3(pawn, tg::g_pawnEyeAngles, pitch, normalizedYaw, 0.0F);
}

// Seeds view history with command-entry angles.
void WriteReplayViewHistory(void* services, void* pawn, float pitch, float yaw)
{
    const float normalizedYaw = NormalizeReplayYaw(yaw);
    WriteVector3(pawn, tg::g_pawnViewAnglePrevious, pitch, normalizedYaw, 0.0F);
    WriteVector3(services, tg::g_servicesOldViewAngles, pitch, normalizedYaw, 0.0F);
}

// Restores drop inputs; retains initial replay positioning.
bool PrepareReplayDropPawn(int slot, void* services, const ReplayDropEvent& event)
{
    const uint64_t generation = PawnGeneration(slot);
    MovementSnapshot pre{};
    if (!services || !ReplayCommandViewSnapshot(slot, pre)) return false;
    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (generation != PawnGeneration(slot)) return false;
    void** vtable = nullptr;
    void* target = nullptr;
    if (!pawn || !SafeRead(pawn, 0, vtable) || !vtable ||
        !SafeRead(vtable, tg::g_vtIdxTeleport * static_cast<int>(sizeof(void*)), target) || !target)
        return false;

    // Teleport updates spatial state for idle drops.
    const float position[3] = { pre.originX, pre.originY, pre.originZ };
    const float velocity[3] = { pre.velX, pre.velY, pre.velZ };
    // Align idle body facing before the native bone read.
    const bool alignStationaryBody = (pre.entityFlags & tg::kFlOnGround) != 0 && std::fabs(pre.velX) < 0.01F && std::fabs(pre.velY) < 0.01F;
    const bool hasRecordedBodyYaw = (event.vectorFlags & ReplayDropBodyYaw) != 0;
    const float bodyYaw = hasRecordedBodyYaw ? event.target[0] : pre.yaw;
    const float angles[3] = { 0.0F, NormalizeReplayYaw(bodyYaw), 0.0F };
    const float* teleportAngles = alignStationaryBody ? angles : nullptr;
    using TeleportFn = void(CS2_SDK_FASTCALL*)(void*, const float*, const float*, const float*);
    reinterpret_cast<TeleportFn>(target)(pawn, position, teleportAngles, velocity);
    if (!IsReplaying(slot) || generation != PawnGeneration(slot)) return false;
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
    if (!bot_controller_hooks::ApplyReplayEyeAngles(slot, pawn, pre.pitch, pre.yaw)) return false;
    pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn) return false;
    WriteRawViewAnglesToPawn(pawn, pre.pitch, pre.yaw);
    return true;
}
} // namespace

// Restores Pawn state before simulation.
bool OnReplayCommandPre(int slot, void* services, const ReplayTick& tick)
{
    if (!ValidSlot(slot) || !services || !g_rep[slot].playing.load(std::memory_order_acquire)) return false;
    const uint64_t generation = PawnGeneration(slot);

    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn || generation != PawnGeneration(slot)) return false;
    if (g_rep[slot].needsInitialTeleport.exchange(false, std::memory_order_acq_rel))
    {
        // Publish the initial origin through Teleport.
        void** vtable = nullptr;
        void* target = nullptr;
        if (!SafeRead(pawn, 0, vtable) || !vtable || !SafeRead(vtable, tg::g_vtIdxTeleport * static_cast<int>(sizeof(void*)), target) ||
            !target)
        {
            BC_LOG_WARN("slot %d: Teleport unavailable", slot);
            StopReplay(slot);
            return false;
        }
        const float position[3] = { tick.pre.originX, tick.pre.originY, tick.pre.originZ };
        const float velocity[3] = { tick.pre.velX, tick.pre.velY, tick.pre.velZ };
        using TeleportFn = void(CS2_SDK_FASTCALL*)(void*, const float*, const float*, const float*);
        reinterpret_cast<TeleportFn>(target)(pawn, position, nullptr, velocity);
        if (!IsReplaying(slot) || generation != PawnGeneration(slot)) return false;
        pawn = input_injector::ResolveReplayPawn(slot, services);
        if (!pawn) return false;
    }
    WriteVelocityToPawn(pawn, tick.pre);
    WriteMovementServiceState(services, tick.pre);

    WriteField(pawn, tg::g_entMoveType, tick.pre.moveType);
    WriteField(pawn, tg::g_entActualMoveType, tick.pre.actualMoveType);
    WriteSceneNodeOrigin(pawn, tick.pre);
    if (!bot_controller_hooks::ApplyReplayEyeAngles(slot, pawn, tick.pre.pitch, tick.pre.yaw)) return false;
    pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn) return false;
    WriteRawViewAnglesToPawn(pawn, tick.pre.pitch, tick.pre.yaw);
    WriteReplayViewHistory(services, pawn, tick.pre.pitch, tick.pre.yaw);
    return IsReplaying(slot) && generation == PawnGeneration(slot);
}

// Restores end state and advances the replay cursor.
void OnReplayCommandPost(int slot, void* services)
{
    if (!ValidSlot(slot) || !services) return;
    ReplayState& p = g_rep[slot];
    if (!p.playing.load(std::memory_order_acquire)) return;
    const uint64_t generation = PawnGeneration(slot);

    ReplayTick t{};
    int cur;
    int total;
    {
        std::scoped_lock lk(p.mu);
        total = static_cast<int>(p.ticks.size());
        cur = p.cursor.load(std::memory_order_relaxed);
        if (cur < 0 || cur >= total) return;
        t = p.ticks[cur];
    }

    void* pawn = input_injector::ResolveReplayPawn(slot, services);
    if (generation != PawnGeneration(slot)) return;
    if (pawn)
    {
        WriteField(pawn, tg::g_entMoveType, t.post.moveType);
        WriteField(pawn, tg::g_entActualMoveType, t.post.actualMoveType);
        // Restore only ground and ducking flags.
        uint32_t live = 0;
        uint32_t mask = tg::kFlOnGround | tg::kFlDucking;
        if (SafeRead(pawn, tg::g_entFlags, live))
        {
            live = (live & ~mask) | (t.post.entityFlags & mask);
            WriteField(pawn, tg::g_entFlags, live);
        }
        if (!bot_controller_hooks::ApplyReplayEyeAngles(slot, pawn, t.post.pitch, t.post.yaw)) return;
    }

    // Revalidate ownership after SetEyeAngles.
    pawn = input_injector::ResolveReplayPawn(slot, services);
    if (!pawn || !IsReplaying(slot) || generation != PawnGeneration(slot)) return;
    WriteVelocityToPawn(pawn, t.post);
    WriteSceneNodeOrigin(pawn, t.post);
    WriteMovementServiceState(services, t.post);

    {
        std::scoped_lock lock(p.mu);
        if (!p.playing.load(std::memory_order_acquire) || generation != PawnGeneration(slot) ||
            p.cursor.load(std::memory_order_relaxed) != cur)
            return;
        p.lastCompletedCursor = cur;
        if (cur + 1 < total)
        {
            p.cursor.store(cur + 1, std::memory_order_relaxed);
            return;
        }
        if (p.loop.load(std::memory_order_relaxed))
        {
            p.needsInitialTeleport.store(true, std::memory_order_release);
            p.lastEventCursor = -1;
            p.cursor.store(0, std::memory_order_relaxed);
            return;
        }
    }
    StopReplay(slot);
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
        {
            std::scoped_lock lk(g_rec[i].mu);
            g_rec[i].ticks.clear();
            g_rec[i].subs.clear();
            g_rec[i].commands.clear();
            g_rec[i].inputHistory.clear();
            g_rec[i].ResetPending();
        }
        {
            std::scoped_lock lk(g_rep[i].mu);
            g_rep[i].generation.fetch_add(1, std::memory_order_release);
            g_rep[i].ticks.clear();
            g_rep[i].subs.clear();
            g_rep[i].commands.clear();
            g_rep[i].inputHistory.clear();
            g_rep[i].subOffset.clear();
            g_rep[i].lastEventCursor = -1;
            g_rep[i].lastCompletedCursor = -1;
        }
        g_rec[i].currentDef.store(-1, std::memory_order_relaxed);
        g_rec[i].liveWs.store(nullptr, std::memory_order_relaxed);
        g_rep[i].cursor.store(0, std::memory_order_relaxed);
        input_injector::ClearReplayPawn(i);
    }
    g_recordingSlots.store(0, std::memory_order_release);
    g_replayingSlots.store(0, std::memory_order_release);
}
} // namespace motion_recorder
} // namespace cs2bc
