// C-ABI exports for CounterStrikeSharp P/Invoke.

#include "dispatch.h"
#include "MotionRecorder.h"
#include "InputInjector.h"
#include "BotController.h"
#include "BuyControllerState.h"
#include "BotProfile.h"
#include "VoiceSender.h"
#include "ProjectileBirthAlign.h"
#include "runtime.h"

#include <tier0/dbg.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#define BC_EXPORT __declspec(dllexport)
#else
#define BC_EXPORT __attribute__((visibility("default")))
#endif

namespace {

constexpr int kRuntimeDisabled = -10;

bool RuntimeEnabled() { return cs2bc::runtime::IsEnabled(); }

} // namespace

extern "C" BC_EXPORT int BotController_Lock(int slot, int kind, int arg)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::dispatch::Lock(slot, static_cast<cs2bc::LockKind>(kind), arg);
}

extern "C" BC_EXPORT int BotController_Unlock(int slot, int kind)
{
    return cs2bc::dispatch::Unlock(slot, static_cast<cs2bc::LockKind>(kind));
}

extern "C" BC_EXPORT int BotController_UnlockAll(int kind) { return cs2bc::dispatch::UnlockAll(static_cast<cs2bc::LockKind>(kind)); }

extern "C" BC_EXPORT int BotController_IsLocked(int slot, int kind)
{
    return cs2bc::dispatch::IsLocked(slot, static_cast<cs2bc::LockKind>(kind));
}

extern "C" BC_EXPORT int BotController_GetVersion() { return 21; }

// Enables/disables the native runtime hook layer while keeping the DLL,
// interfaces, schema, commands and C ABI loaded.
extern "C" BC_EXPORT int BotController_SetRuntimeEnabled(int enabled)
{
    char error[256] = { 0 };
    if (cs2bc::runtime::SetEnabled(enabled != 0, error, sizeof(error))) return 0;

    Warning("[BotController] SetRuntimeEnabled(%d) failed: %s\n", enabled != 0 ? 1 : 0, error[0] ? error : "unknown error");
    return -1;
}

extern "C" BC_EXPORT int BotController_IsRuntimeEnabled() { return cs2bc::runtime::IsEnabled() ? 1 : 0; }

extern "C" BC_EXPORT int BotController_IsRuntimePrepared() { return cs2bc::runtime::IsPrepared() ? 1 : 0; }

// Configures native projectile birth fields for the current server build
extern "C" BC_EXPORT int BotController_SetProjectileBirthAlignOffsets(int initialPositionOffset, int initialVelocityOffset)
{
    return cs2bc::projectile_birth_align::ConfigureOffsets(initialPositionOffset, initialVelocityOffset);
}

// Queues one projectile's recorded birth position and velocity
extern "C" BC_EXPORT int BotController_QueueProjectileBirthAlign(
    int slot, uint64_t entityPtr, float posX, float posY, float posZ, float velX, float velY, float velZ)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;

    // IsReplaying() now also validates that the slot still belongs to a live bot.
    if (!cs2bc::motion_recorder::IsReplaying(slot)) return -3;

    return cs2bc::projectile_birth_align::Queue(entityPtr, posX, posY, posZ, velX, velY, velZ);
}

// Clears pending native projectile alignment writes
extern "C" BC_EXPORT int BotController_ClearProjectileBirthAlign() { return cs2bc::projectile_birth_align::Clear(); }

// Returns native projectile alignment diagnostics
extern "C" BC_EXPORT int BotController_GetProjectileBirthAlignStatus(cs2bc::projectile_birth_align::Status* out, int size)
{
    return cs2bc::projectile_birth_align::GetStatus(out, size);
}

// Create an independently cancellable usercmd injection
extern "C" BC_EXPORT int64_t BotController_InjectUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!RuntimeEnabled()) return -1;
    return cs2bc::input_injector::InjectUsercmd(slot, buttonMask, durationMs);
}

// Create an independently cancellable persistent analog movement override
extern "C" BC_EXPORT int64_t BotController_StartUsercmdMovement(int slot, float forwardMove, float leftMove)
{
    if (!RuntimeEnabled()) return -1;
    return cs2bc::input_injector::StartUsercmdMovement(slot, forwardMove, leftMove);
}

// Update one persistent analog movement override
extern "C" BC_EXPORT int BotController_UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::input_injector::UpdateUsercmdMovement(slot, movementId, forwardMove, leftMove) ? 0 : -1;
}

// Cancel one persistent analog movement override
extern "C" BC_EXPORT int BotController_CancelUsercmdMovement(int slot, int64_t movementId)
{
    return cs2bc::input_injector::CancelUsercmdMovement(slot, movementId) ? 0 : -1;
}

// Cancel one usercmd injection by its token
extern "C" BC_EXPORT int BotController_CancelUsercmdInjection(int slot, int64_t injectionId)
{
    return cs2bc::input_injector::CancelUsercmdInjection(slot, injectionId) ? 0 : -1;
}

// Suppress selected usercmd buttons for a fixed duration
extern "C" BC_EXPORT int BotController_SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::input_injector::SuppressUsercmd(slot, buttonMask, durationMs) ? 0 : -1;
}

// Create an independently cancellable persistent usercmd suppression
extern "C" BC_EXPORT int64_t BotController_StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
    if (!RuntimeEnabled()) return -1;
    return cs2bc::input_injector::StartUsercmdSuppression(slot, buttonMask);
}

// Cancel one persistent usercmd suppression by its token
extern "C" BC_EXPORT int BotController_CancelUsercmdSuppression(int slot, int64_t suppressionId)
{
    return cs2bc::input_injector::CancelUsercmdSuppression(slot, suppressionId) ? 0 : -1;
}

// Return 1 when the plugin can allocate and send voice net messages.
extern "C" BC_EXPORT int BotController_CanSendVoice() { return cs2bc::voice_sender::IsAvailable() ? 1 : 0; }

// Return 0 when voice sending is ready, otherwise a negative setup code.
extern "C" BC_EXPORT int BotController_GetVoiceStatus() { return cs2bc::voice_sender::GetStatus(); }

// Send one encoded Opus voice frame to a recipient player slot.
extern "C" BC_EXPORT int BotController_SendVoiceFrame(int recipientSlot,
                                                      int senderClient,
                                                      uint64_t senderXuid,
                                                      const uint8_t* audio,
                                                      int audioBytes,
                                                      int sampleRate,
                                                      float voiceLevel,
                                                      int sequenceBytes,
                                                      int sectionNumber,
                                                      int uncompressedSampleOffset,
                                                      uint32_t numPackets,
                                                      const uint32_t* packetOffsets,
                                                      int packetOffsetCount,
                                                      int tick,
                                                      int audibleMask)
{
    return cs2bc::voice_sender::SendVoiceFrame(recipientSlot, senderClient, senderXuid, audio, audioBytes, sampleRate, voiceLevel,
                                               sequenceBytes, sectionNumber, uncompressedSampleOffset, numPackets, packetOffsets,
                                               packetOffsetCount, tick, audibleMask);
}

// Read a bot's BotProfile by slot. 0 ok / -1 no live bot or null profile.
extern "C" BC_EXPORT int BotController_GetProfile(int slot, cs2bc::BotProfileData* out)
{
    if (!out) return -1;
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::bot_profile::ReadProfile(slot, *out) ? 0 : -1;
}

// ---- Bot buy plans ----

namespace {

// Split a space/comma separated alias string into tokens.
std::vector<std::string> SplitAliases(const char* csv)
{
    std::vector<std::string> out;
    if (!csv) return out;
    std::string cur;
    for (const char* p = csv; *p; ++p)
    {
        char c = *p;
        if (c == ' ' || c == ',' || c == '\t')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

// Set a slot's buy plan from a space/comma separated alias list. 0 ok.
extern "C" BC_EXPORT int BotController_SetBuyPlan(int slot, const char* aliases)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    if (!cs2bc::bot_controller_hooks::IsLiveBotSlot(slot)) return -4;
    if (slot < 0 || slot >= cs2bc::buy_controller_state::kMaxSlots) return -2;
    cs2bc::buy_controller_state::Set(slot, SplitAliases(aliases), false);
    return 0;
}

// Mark a slot to buy nothing this round. 0 ok.
extern "C" BC_EXPORT int BotController_SetBuySkip(int slot)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    if (!cs2bc::bot_controller_hooks::IsLiveBotSlot(slot)) return -4;
    if (slot < 0 || slot >= cs2bc::buy_controller_state::kMaxSlots) return -2;
    cs2bc::buy_controller_state::Set(slot, {}, true);
    return 0;
}

extern "C" BC_EXPORT int BotController_ClearBuyPlan(int slot)
{
    if (slot < 0 || slot >= cs2bc::buy_controller_state::kMaxSlots) return -2;
    cs2bc::buy_controller_state::Clear(slot);
    return 0;
}

extern "C" BC_EXPORT int BotController_ClearAllBuyPlans()
{
    cs2bc::buy_controller_state::ClearAll();
    return 0;
}

// Item count for a slot's plan: -1 none, 0 skip/empty, >0 alias count.
extern "C" BC_EXPORT int BotController_GetBuyPlanItemCount(int slot) { return cs2bc::buy_controller_state::ItemCount(slot); }

// ---- Motion recording & replay ----

// Begin/stop recording a human slot's per-tick movement. 0 ok / -1 fail.
extern "C" BC_EXPORT int BotController_StartRecord(int slot)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::motion_recorder::StartRecord(slot) ? 0 : -1;
}

extern "C" BC_EXPORT int BotController_StopRecord(int slot) { return cs2bc::motion_recorder::StopRecord(slot) ? 0 : -1; }

// Recorded tick / subtick counts for a slot. <0 on bad slot.
extern "C" BC_EXPORT int BotController_GetRecordedTickCount(int slot) { return cs2bc::motion_recorder::RecordedTickCount(slot); }

extern "C" BC_EXPORT int BotController_GetRecordedSubtickCount(int slot) { return cs2bc::motion_recorder::RecordedSubtickCount(slot); }

// Recorded command-frame count for a slot
extern "C" BC_EXPORT int BotController_GetRecordedCommandCount(int slot) { return cs2bc::motion_recorder::RecordedCommandCount(slot); }

// Copy recorded ticks / subticks into caller buffers. Returns count written.
extern "C" BC_EXPORT int BotController_CopyRecordedTicks(int slot, cs2bc::ReplayTick* out, int maxTicks)
{
    return cs2bc::motion_recorder::CopyTicks(slot, out, maxTicks);
}

extern "C" BC_EXPORT int BotController_CopyRecordedSubticks(int slot, cs2bc::SubtickMove* out, int maxSubticks)
{
    return cs2bc::motion_recorder::CopySubticks(slot, out, maxSubticks);
}

// Copies recorded command frames into a caller-owned buffer
extern "C" BC_EXPORT int BotController_CopyRecordedCommands(int slot, cs2bc::ReplayCommandFrameData* out, int maxCommands)
{
    return cs2bc::motion_recorder::CopyCommands(slot, out, maxCommands);
}

// Load parallel tick + subtick arrays into a slot's replay buffer. 0 ok.
extern "C" BC_EXPORT int
BotController_LoadReplay(int slot, const cs2bc::ReplayTick* ticks, int tickCount, const cs2bc::SubtickMove* subs, int subCount) noexcept
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::motion_recorder::LoadReplay(slot, ticks, tickCount, subs, subCount) ? 0 : -1;
}

// Load replay buffers with optional per-tick command and movement data
extern "C" BC_EXPORT int BotController_LoadReplayExtended(int slot,
                                                          const cs2bc::ReplayTick* ticks,
                                                          int tickCount,
                                                          const cs2bc::SubtickMove* subs,
                                                          int subCount,
                                                          const cs2bc::ReplayCommandFrameData* commands,
                                                          int commandCount,
                                                          const cs2bc::ReplayMovementExtra* movementExtras,
                                                          int movementExtraCount) noexcept
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::motion_recorder::LoadReplayExtended(slot, ticks, tickCount, subs, subCount, commands, commandCount, movementExtras,
                                                      movementExtraCount)
               ? 0
               : -1;
}

// Move a slot's just-recorded buffers into another slot's replay buffer
extern "C" BC_EXPORT int BotController_TransferRecordingToReplay(int srcSlot, int dstSlot)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;

    int nt = cs2bc::motion_recorder::RecordedTickCount(srcSlot);
    if (nt <= 0) return -1;
    int ns = cs2bc::motion_recorder::RecordedSubtickCount(srcSlot);
    ns = std::max(ns, 0);
    int nc = cs2bc::motion_recorder::RecordedCommandCount(srcSlot);
    if (nc != nt) return -1;
    std::vector<cs2bc::ReplayTick> ticks(nt);
    std::vector<cs2bc::SubtickMove> subs(ns > 0 ? ns : 1);
    std::vector<cs2bc::ReplayCommandFrameData> commands(nc);
    int gotT = cs2bc::motion_recorder::CopyTicks(srcSlot, ticks.data(), nt);
    int gotS = ns > 0 ? cs2bc::motion_recorder::CopySubticks(srcSlot, subs.data(), ns) : 0;
    int gotC = cs2bc::motion_recorder::CopyCommands(srcSlot, commands.data(), nc);
    if (gotT <= 0 || gotC != gotT) return -1;
    return cs2bc::motion_recorder::LoadReplayExtended(dstSlot, ticks.data(), gotT, subs.data(), gotS, commands.data(), gotC, nullptr, 0)
               ? 0
               : -1;
}

extern "C" BC_EXPORT int BotController_StartReplay(int slot, int loop)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::motion_recorder::StartReplay(slot, loop != 0) ? 0 : -1;
}

// Registers the managed plugin's authoritative pawn pointer for replay.
extern "C" BC_EXPORT int BotController_SetReplayPawn(int slot, uint64_t pawnPtr)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;

    void* pawn = reinterpret_cast<void*>(static_cast<uintptr_t>(pawnPtr)); // NOLINT(performance-no-int-to-ptr)
    return cs2bc::input_injector::SetReplayPawn(slot, pawn) ? 0 : -1;
}

extern "C" BC_EXPORT int BotController_StopReplay(int slot) { return cs2bc::motion_recorder::StopReplay(slot) ? 0 : -1; }

// Current replay tick index, or <0 if the slot is not replaying.
extern "C" BC_EXPORT int BotController_GetReplayCursor(int slot) { return cs2bc::motion_recorder::ReplayCursor(slot); }

// Total ticks loaded in a slot's replay buffer.
extern "C" BC_EXPORT int BotController_GetReplayTotal(int slot) { return cs2bc::motion_recorder::ReplayTotal(slot); }

// Copy the tick currently being replayed (for C# to drive weapon/fire).
// Returns 0 on success, -1 if the slot isn't replaying.
extern "C" BC_EXPORT int BotController_GetReplayTick(int slot, cs2bc::ReplayTick* out)
{
    if (!out) return -1;
    return cs2bc::motion_recorder::CurrentReplayTick(slot, *out) ? 0 : -1;
}

// Switch a bot to the weapon with this def index
// Returns 0 ok / -1 not found or bot not ready.
extern "C" BC_EXPORT int BotController_SwitchBotWeapon(int slot, int defIndex)
{
    if (!RuntimeEnabled()) return kRuntimeDisabled;
    return cs2bc::motion_recorder::SwitchBotWeaponByDef(slot, defIndex) ? 0 : -1;
}

// Def index of the bot's current active weapon (same normalization as the
// recorded WeaponDefIndex). <0 if unresolved. For C# to reconcile replay.
extern "C" BC_EXPORT int BotController_GetBotActiveWeaponDef(int slot)
{
    if (!RuntimeEnabled()) return -1;
    return cs2bc::motion_recorder::BotActiveWeaponDef(slot);
}

extern "C" BC_EXPORT uint64_t BotController_GetHookCallCount() { return cs2bc::input_injector::HookCallCount(); }

extern "C" BC_EXPORT int BotController_GetLastResolvedSlot() { return cs2bc::input_injector::LastResolvedSlot(); }

extern "C" BC_EXPORT uint64_t BotController_GetFinishMoveCallCount() { return cs2bc::input_injector::FinishMoveCallCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetPlayerRunCommandCallCount() { return cs2bc::input_injector::PlayerRunCommandCallCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetPhysicsSimulateCallCount() { return cs2bc::input_injector::PhysicsSimulateCallCount(); }

extern "C" BC_EXPORT int BotController_GetLastPhysicsSlot() { return cs2bc::input_injector::LastPhysicsSlot(); }

extern "C" BC_EXPORT uint64_t BotController_GetReplayCommitCount() { return cs2bc::input_injector::ReplayCommitCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetSlotResolveCallCount() { return cs2bc::input_injector::SlotResolveCallCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetSlotResolveFailureCount() { return cs2bc::input_injector::SlotResolveFailureCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetLastServices() { return static_cast<uint64_t>(cs2bc::input_injector::LastServices()); }

extern "C" BC_EXPORT uint64_t BotController_GetLastPawn() { return static_cast<uint64_t>(cs2bc::input_injector::LastPawn()); }

extern "C" BC_EXPORT uint32_t BotController_GetLastControllerHandle() { return cs2bc::input_injector::LastControllerHandle(); }

extern "C" BC_EXPORT uint32_t BotController_GetLastOriginalControllerHandle()
{
    return cs2bc::input_injector::LastOriginalControllerHandle();
}

extern "C" BC_EXPORT int BotController_GetLastControllerIndex() { return cs2bc::input_injector::LastControllerIndex(); }

extern "C" BC_EXPORT int BotController_GetLastOriginalControllerIndex() { return cs2bc::input_injector::LastOriginalControllerIndex(); }

extern "C" BC_EXPORT int BotController_GetLastOwnerSlot() { return cs2bc::input_injector::LastOwnerSlot(); }

extern "C" BC_EXPORT int BotController_IsLiveBotSlot(int slot)
{
    if (!RuntimeEnabled()) return 0;

    return cs2bc::bot_controller_hooks::IsLiveBotSlot(slot) ? 1 : 0;
}
