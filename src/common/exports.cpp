// C-ABI exports for CounterStrikeSharp P/Invoke. quiet=true on all entries.

#include "dispatch.h"
#include "MotionRecorder.h"
#include "InputInjector.h"
#include "BuyControllerState.h"
#include "BotProfile.h"
#include "VoiceSender.h"

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#define BC_EXPORT __declspec(dllexport)
#else
#define BC_EXPORT __attribute__((visibility("default")))
#endif

extern "C" BC_EXPORT int BotController_Lock(int slot, int kind, int arg)
{
    return BotController::Dispatch::Lock(slot,
                                         static_cast<BotController::LockKind>(kind), arg, /*quiet=*/true);
}

extern "C" BC_EXPORT int BotController_Unlock(int slot, int kind)
{
    return BotController::Dispatch::Unlock(slot,
                                           static_cast<BotController::LockKind>(kind), /*quiet=*/true);
}

extern "C" BC_EXPORT int BotController_UnlockAll(int kind)
{
    return BotController::Dispatch::UnlockAll(
        static_cast<BotController::LockKind>(kind), /*quiet=*/true);
}

extern "C" BC_EXPORT int BotController_IsLocked(int slot, int kind)
{
    return BotController::Dispatch::IsLocked(slot,
                                             static_cast<BotController::LockKind>(kind));
}

extern "C" BC_EXPORT int BotController_GetVersion()
{
    return 14;
}

// Return 1 when the plugin can allocate and send voice net messages.
extern "C" BC_EXPORT int BotController_CanSendVoice()
{
    return BotController::VoiceSender::IsAvailable() ? 1 : 0;
}

// Return 0 when voice sending is ready, otherwise a negative setup code.
extern "C" BC_EXPORT int BotController_GetVoiceStatus()
{
    return BotController::VoiceSender::GetStatus();
}

// Send one encoded Opus voice frame to a recipient player slot.
extern "C" BC_EXPORT int BotController_SendVoiceFrame(
    int recipientSlot,
    int senderClient,
    uint64_t senderXuid,
    const uint8_t *audio,
    int audioBytes,
    int sampleRate,
    float voiceLevel,
    int sequenceBytes,
    int sectionNumber,
    int uncompressedSampleOffset,
    uint32_t numPackets,
    const uint32_t *packetOffsets,
    int packetOffsetCount,
    int tick,
    int audibleMask)
{
    return BotController::VoiceSender::SendVoiceFrame(
        recipientSlot,
        senderClient,
        senderXuid,
        audio,
        audioBytes,
        sampleRate,
        voiceLevel,
        sequenceBytes,
        sectionNumber,
        uncompressedSampleOffset,
        numPackets,
        packetOffsets,
        packetOffsetCount,
        tick,
        audibleMask);
}

// Read a bot's BotProfile by slot. 0 ok / -1 no live bot or null profile.
extern "C" BC_EXPORT int BotController_GetProfile(int slot, BotController::BotProfileData *out)
{
    if (!out)
        return -1;
    return BotController::BotProfile::ReadProfile(slot, *out) ? 0 : -1;
}

// ---- Bot buy plans ----

// Split a space/comma separated alias string into tokens.
static std::vector<std::string> SplitAliases(const char *csv)
{
    std::vector<std::string> out;
    if (!csv)
        return out;
    std::string cur;
    for (const char *p = csv; *p; ++p)
    {
        char c = *p;
        if (c == ' ' || c == ',' || c == '\t')
        {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Set a slot's buy plan from a space/comma separated alias list. 0 ok.
extern "C" BC_EXPORT int BotController_SetBuyPlan(int slot, const char *aliases)
{
    if (slot < 0 || slot >= BotController::BuyControllerState::kMaxSlots)
        return -2;
    BotController::BuyControllerState::Set(slot, SplitAliases(aliases), false);
    return 0;
}

// Mark a slot to buy nothing this round. 0 ok.
extern "C" BC_EXPORT int BotController_SetBuySkip(int slot)
{
    if (slot < 0 || slot >= BotController::BuyControllerState::kMaxSlots)
        return -2;
    BotController::BuyControllerState::Set(slot, {}, true);
    return 0;
}

extern "C" BC_EXPORT int BotController_ClearBuyPlan(int slot)
{
    if (slot < 0 || slot >= BotController::BuyControllerState::kMaxSlots)
        return -2;
    BotController::BuyControllerState::Clear(slot);
    return 0;
}

extern "C" BC_EXPORT int BotController_ClearAllBuyPlans()
{
    BotController::BuyControllerState::ClearAll();
    return 0;
}

// Item count for a slot's plan: -1 none, 0 skip/empty, >0 alias count.
extern "C" BC_EXPORT int BotController_GetBuyPlanItemCount(int slot)
{
    return BotController::BuyControllerState::ItemCount(slot);
}

// ---- Motion recording & replay ----

// Begin/stop recording a human slot's per-tick movement. 0 ok / -1 fail.
extern "C" BC_EXPORT int BotController_StartRecord(int slot)
{
    return BotController::MotionRecorder::StartRecord(slot) ? 0 : -1;
}

extern "C" BC_EXPORT int BotController_StopRecord(int slot)
{
    return BotController::MotionRecorder::StopRecord(slot) ? 0 : -1;
}

// Recorded tick / subtick counts for a slot. <0 on bad slot.
extern "C" BC_EXPORT int BotController_GetRecordedTickCount(int slot)
{
    return BotController::MotionRecorder::RecordedTickCount(slot);
}

extern "C" BC_EXPORT int BotController_GetRecordedSubtickCount(int slot)
{
    return BotController::MotionRecorder::RecordedSubtickCount(slot);
}

// Copy recorded ticks / subticks into caller buffers. Returns count written.
extern "C" BC_EXPORT int BotController_CopyRecordedTicks(int slot, BotController::ReplayTick *out, int maxTicks)
{
    return BotController::MotionRecorder::CopyTicks(slot, out, maxTicks);
}

extern "C" BC_EXPORT int BotController_CopyRecordedSubticks(int slot, BotController::SubtickMove *out, int maxSubticks)
{
    return BotController::MotionRecorder::CopySubticks(slot, out, maxSubticks);
}

// Load parallel tick + subtick arrays into a slot's replay buffer. 0 ok.
extern "C" BC_EXPORT int BotController_LoadReplay(int slot,
                                                  const BotController::ReplayTick *ticks, int tickCount,
                                                  const BotController::SubtickMove *subs, int subCount) noexcept
{
    return BotController::MotionRecorder::LoadReplay(slot, ticks, tickCount,
                                                     subs, subCount)
               ? 0
               : -1;
}

// Load replay buffers with optional per-tick command and movement data
extern "C" BC_EXPORT int BotController_LoadReplayExtended(
    int slot,
    const BotController::ReplayTick *ticks, int tickCount,
    const BotController::SubtickMove *subs, int subCount,
    const BotController::ReplayCommandFrameData *commands, int commandCount,
    const BotController::ReplayMovementExtra *movementExtras,
    int movementExtraCount) noexcept
{
    return BotController::MotionRecorder::LoadReplayExtended(
               slot, ticks, tickCount, subs, subCount,
               commands, commandCount, movementExtras, movementExtraCount)
               ? 0
               : -1;
}

// Move a slot's just-recorded buffers into another slot's replay buffer
extern "C" BC_EXPORT int BotController_TransferRecordingToReplay(int srcSlot, int dstSlot)
{
    int nt = BotController::MotionRecorder::RecordedTickCount(srcSlot);
    if (nt <= 0)
        return -1;
    int ns = BotController::MotionRecorder::RecordedSubtickCount(srcSlot);
    if (ns < 0)
        ns = 0;
    std::vector<BotController::ReplayTick> ticks(nt);
    std::vector<BotController::SubtickMove> subs(ns > 0 ? ns : 1);
    int gotT = BotController::MotionRecorder::CopyTicks(srcSlot, ticks.data(), nt);
    int gotS = ns > 0
                   ? BotController::MotionRecorder::CopySubticks(srcSlot, subs.data(), ns)
                   : 0;
    if (gotT <= 0)
        return -1;
    return BotController::MotionRecorder::LoadReplayExtended(
               dstSlot, ticks.data(), gotT, subs.data(), gotS,
               nullptr, 0, nullptr, 0)
               ? 0
               : -1;
}

extern "C" BC_EXPORT int BotController_StartReplay(int slot, int loop)
{
    return BotController::MotionRecorder::StartReplay(slot, loop != 0) ? 0 : -1;
}

// Registers the managed plugin's authoritative pawn pointer for replay.
extern "C" BC_EXPORT int BotController_SetReplayPawn(int slot, uint64_t pawnPtr)
{
    return BotController::InputInjector::SetReplayPawn(
               slot,
               reinterpret_cast<void *>(static_cast<uintptr_t>(pawnPtr)))
               ? 0
               : -1;
}

extern "C" BC_EXPORT int BotController_StopReplay(int slot)
{
    return BotController::MotionRecorder::StopReplay(slot) ? 0 : -1;
}

// Current replay tick index, or <0 if the slot is not replaying.
extern "C" BC_EXPORT int BotController_GetReplayCursor(int slot)
{
    return BotController::MotionRecorder::ReplayCursor(slot);
}

// Total ticks loaded in a slot's replay buffer.
extern "C" BC_EXPORT int BotController_GetReplayTotal(int slot)
{
    return BotController::MotionRecorder::ReplayTotal(slot);
}

// Copy the tick currently being replayed (for C# to drive weapon/fire).
// Returns 0 on success, -1 if the slot isn't replaying.
extern "C" BC_EXPORT int BotController_GetReplayTick(int slot, BotController::ReplayTick *out)
{
    if (!out)
        return -1;
    return BotController::MotionRecorder::CurrentReplayTick(slot, *out) ? 0 : -1;
}

// Switch a bot to the weapon with this def index
// Returns 0 ok / -1 not found or bot not ready.
extern "C" BC_EXPORT int BotController_SwitchBotWeapon(int slot, int defIndex)
{
    return BotController::MotionRecorder::SwitchBotWeaponByDef(slot, defIndex) ? 0 : -1;
}

// Def index of the bot's current active weapon (same normalization as the
// recorded WeaponDefIndex). <0 if unresolved. For C# to reconcile replay.
extern "C" BC_EXPORT int BotController_GetBotActiveWeaponDef(int slot)
{
    return BotController::MotionRecorder::BotActiveWeaponDef(slot);
}

extern "C" BC_EXPORT uint64_t BotController_GetHookCallCount()
{
    return BotController::InputInjector::HookCallCount();
}

extern "C" BC_EXPORT int BotController_GetLastResolvedSlot()
{
    return BotController::InputInjector::LastResolvedSlot();
}

extern "C" BC_EXPORT uint64_t BotController_GetFinishMoveCallCount()
{
    return BotController::InputInjector::FinishMoveCallCount();
}

extern "C" BC_EXPORT uint64_t BotController_GetPlayerRunCommandCallCount()
{
    return BotController::InputInjector::PlayerRunCommandCallCount();
}

extern "C" BC_EXPORT uint64_t BotController_GetPhysicsSimulateCallCount()
{
    return BotController::InputInjector::PhysicsSimulateCallCount();
}

extern "C" BC_EXPORT int BotController_GetLastPhysicsSlot()
{
    return BotController::InputInjector::LastPhysicsSlot();
}

extern "C" BC_EXPORT uint64_t BotController_GetReplayCommitCount()
{
    return BotController::InputInjector::ReplayCommitCount();
}

extern "C" BC_EXPORT uint64_t BotController_GetSlotResolveCallCount()
{
    return BotController::InputInjector::SlotResolveCallCount();
}

extern "C" BC_EXPORT uint64_t BotController_GetSlotResolveFailureCount()
{
    return BotController::InputInjector::SlotResolveFailureCount();
}

extern "C" BC_EXPORT uint64_t BotController_GetLastServices()
{
    return static_cast<uint64_t>(BotController::InputInjector::LastServices());
}

extern "C" BC_EXPORT uint64_t BotController_GetLastPawn()
{
    return static_cast<uint64_t>(BotController::InputInjector::LastPawn());
}

extern "C" BC_EXPORT uint32_t BotController_GetLastControllerHandle()
{
    return BotController::InputInjector::LastControllerHandle();
}

extern "C" BC_EXPORT uint32_t BotController_GetLastOriginalControllerHandle()
{
    return BotController::InputInjector::LastOriginalControllerHandle();
}

extern "C" BC_EXPORT int BotController_GetLastControllerIndex()
{
    return BotController::InputInjector::LastControllerIndex();
}

extern "C" BC_EXPORT int BotController_GetLastOriginalControllerIndex()
{
    return BotController::InputInjector::LastOriginalControllerIndex();
}

extern "C" BC_EXPORT int BotController_GetLastOwnerSlot()
{
    return BotController::InputInjector::LastOwnerSlot();
}
