// C-ABI exports for CounterStrikeSharp P/Invoke.

#include "dispatch.h"
#include "MotionRecorder.h"
#include "InputInjector.h"
#include "BuyControllerState.h"
#include "BotProfile.h"
#include "VoiceSender.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#define BC_EXPORT __declspec(dllexport)
#else
#define BC_EXPORT __attribute__((visibility("default")))
#endif

extern "C" BC_EXPORT int BotController_Lock(int slot, int kind, int arg)
{
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

// ABI 21 keeps the native command-based projectile replay path without birth alignment exports.
extern "C" BC_EXPORT int BotController_GetVersion() { return 22; }

// Create an independently cancellable usercmd injection
extern "C" BC_EXPORT int64_t BotController_InjectUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    return cs2bc::input_injector::InjectUsercmd(slot, buttonMask, durationMs);
}

// Create an independently cancellable persistent analog movement override
extern "C" BC_EXPORT int64_t BotController_StartUsercmdMovement(int slot, float forwardMove, float leftMove)
{
    return cs2bc::input_injector::StartUsercmdMovement(slot, forwardMove, leftMove);
}

// Update one persistent analog movement override
extern "C" BC_EXPORT int BotController_UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove)
{
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
    return cs2bc::input_injector::SuppressUsercmd(slot, buttonMask, durationMs) ? 0 : -1;
}

// Create an independently cancellable persistent usercmd suppression
extern "C" BC_EXPORT int64_t BotController_StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
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
    if (slot < 0 || slot >= cs2bc::buy_controller_state::kMaxSlots) return -2;
    cs2bc::buy_controller_state::Set(slot, SplitAliases(aliases), false);
    return 0;
}

// Mark a slot to buy nothing this round. 0 ok.
extern "C" BC_EXPORT int BotController_SetBuySkip(int slot)
{
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
extern "C" BC_EXPORT int BotController_StartRecord(int slot) { return cs2bc::motion_recorder::StartRecord(slot) ? 0 : -1; }

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

// Load replay buffers with optional per-tick command and movement data. 0 ok.
extern "C" BC_EXPORT int BotController_LoadReplay(int slot,
                                                  const cs2bc::ReplayTick* ticks,
                                                  int tickCount,
                                                  const cs2bc::SubtickMove* subs,
                                                  int subCount,
                                                  const cs2bc::ReplayCommandFrameData* commands,
                                                  int commandCount,
                                                  const cs2bc::ReplayMovementExtra* movementExtras,
                                                  int movementExtraCount) noexcept
{
    return cs2bc::motion_recorder::LoadReplay(slot, ticks, tickCount, subs, subCount, commands, commandCount, movementExtras,
                                              movementExtraCount)
               ? 0
               : -1;
}

// Move a slot's just-recorded buffers into another slot's replay buffer
extern "C" BC_EXPORT int BotController_TransferRecordingToReplay(int srcSlot, int dstSlot)
{
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
    return cs2bc::motion_recorder::LoadReplay(dstSlot, ticks.data(), gotT, subs.data(), gotS, commands.data(), gotC, nullptr, 0) ? 0 : -1;
}

extern "C" BC_EXPORT int BotController_StartReplay(int slot, int loop)
{
    return cs2bc::motion_recorder::StartReplay(slot, loop != 0) ? 0 : -1;
}

// Registers the managed plugin's authoritative pawn pointer for replay.
extern "C" BC_EXPORT int BotController_SetReplayPawn(int slot, uint64_t pawnPtr)
{
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
    return cs2bc::motion_recorder::SwitchBotWeaponByDef(slot, defIndex) ? 0 : -1;
}

// Def index of the bot's current active weapon (same normalization as the
// recorded WeaponDefIndex). <0 if unresolved. For C# to reconcile replay.
extern "C" BC_EXPORT int BotController_GetBotActiveWeaponDef(int slot) { return cs2bc::motion_recorder::BotActiveWeaponDef(slot); }
