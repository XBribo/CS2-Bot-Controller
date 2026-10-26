// Motion recording & replay

#pragma once

#include "cs_usercmd.pb.h"

#include <cstdint>
#include <string>

namespace cs2bc {
// Pawn state at command entry and completion.
#pragma pack(push, 4)
struct MovementSnapshot
{
    float originX, originY, originZ; // scene node m_vecAbsOrigin
    float velX, velY, velZ; // m_vecAbsVelocity
    float pitch, yaw, roll; // view angles
    uint32_t entityFlags; // m_fFlags (bit0 = FL_ONGROUND, bit1 = FL_DUCKING)
    uint8_t moveType; // m_MoveType (MoveType_t)
    uint8_t pad[3]; // keep 4-byte alignment explicit
    uint64_t buttons; // services button states[0] (pressed)
    uint64_t buttons1; // states[1]
    uint64_t buttons2; // states[2]
    float duckAmount; // m_flDuckAmount (0=stand, 1=full crouch)
    float duckSpeed; // m_flDuckSpeed
    float ladderNormalX; // m_vecLadderNormal (ladder anim facing)
    float ladderNormalY;
    float ladderNormalZ;
    uint8_t ducked; // m_bDucked
    uint8_t ducking; // m_bDucking
    uint8_t desiresDuck; // m_bDesiresDuck
    uint8_t actualMoveType; // m_nActualMoveType (networked, ladder anim)
};

// One command; ReplayTick retains its ABI layout.
struct ReplayTick
{
    MovementSnapshot pre;
    MovementSnapshot post;
    int32_t weaponDefIndex; // active weapon item-def index, -1 = none
    uint32_t numSubtick; // subtick moves for this tick, 0..36
    uint32_t eventFlags; // ReplayEventFlags bitmask
    int32_t eventWeaponDefIndex; // active item captured for the event, -1 = none
    uint32_t eventDropVectorFlags; // ReplayDropVectorFlags bitmask, including ReplayDropBodyYaw
    float eventDropTargetX; // body yaw when ReplayDropBodyYaw is set and target is absent
    float eventDropTargetY;
    float eventDropTargetZ;
    float eventDropVelocityX;
    float eventDropVelocityY;
    float eventDropVelocityZ;
    float eventDropReleaseX;
    float eventDropReleaseY;
    float eventDropReleaseZ;
    float eventDropReleaseQuatX;
    float eventDropReleaseQuatY;
    float eventDropReleaseQuatZ;
    float eventDropReleaseQuatW;
};

enum ReplayEventFlags : uint32_t // NOLINT(performance-enum-size)
{
    ReplayEventNone = 0,
    ReplayEventDrop = 1U << 0,
};

enum ReplayDropVectorFlags : uint32_t // NOLINT(performance-enum-size)
{
    ReplayDropVectorNone = 0,
    ReplayDropVectorTarget = 1U << 0,
    ReplayDropVectorVelocity = 1U << 1,
    ReplayDropBodyYaw = 1U << 2, // target[0] stores scene-node absolute yaw when target is absent
    ReplayDropReleasePose = 1U << 3,
};

struct ReplayDropEvent
{
    int weaponDefIndex;
    uint32_t vectorFlags;
    float target[3];
    float velocity[3];
    float releasePosition[3];
    float releaseQuaternion[4];
};

struct SubtickMove
{
    float when; // [0,1) time within the tick
    uint32_t button; // 0 = analog, else engine button bit
    float pressed; // digital: 1=down 0=up (stored as float)
    float analogForward; // analog_forward_delta
    float analogLeft; // analog_left_delta
    float pitchDelta; // pitch_delta
    float yawDelta; // yaw_delta
};

struct ReplayCommandFrameData
{
    float forwardMove;
    float leftMove;
    float upMove;
    float pitch;
    float yaw;
    float roll;
    uint64_t buttons;
    uint64_t buttons1;
    uint64_t buttons2;
    int32_t mouseDx;
    int32_t mouseDy;
    int32_t weaponSelect;
    uint32_t fields;
    uint8_t leftHandDesired;
    uint8_t pad[3];
};

#pragma pack(pop)

static_assert(sizeof(ReplayCommandFrameData) == 68);
static_assert(sizeof(ReplayTick) == 256);

namespace motion_recorder {
constexpr int kMaxSlots = 64;
constexpr int kMaxSubtickPerTick = 36;
constexpr uint32_t kCommandFieldForwardMove = 1U << 0;
constexpr uint32_t kCommandFieldLeftMove = 1U << 1;
constexpr uint32_t kCommandFieldUpMove = 1U << 2;
constexpr uint32_t kCommandFieldViewAngles = 1U << 3;
constexpr uint32_t kCommandFieldButtons = 1U << 4;
constexpr uint32_t kCommandFieldMouse = 1U << 5;
constexpr uint32_t kCommandFieldWeaponSelect = 1U << 6;
constexpr uint32_t kCommandFieldLeftHand = 1U << 7;
constexpr uint32_t kCommandFieldWeaponSelectDef = 1U << 8;

// Complete replay command input.
struct ReplayCommandFrame
{
    ReplayTick tick;
    SubtickMove subticks[kMaxSubtickPerTick];
    int32_t subtickCount;
    int32_t weaponSelect;
    MovementSnapshot commandView;
    uint64_t buttons0;
    uint64_t buttons1;
    uint64_t buttons2;
    uint32_t commandFields;
    float forwardMove;
    float leftMove;
    float upMove;
    int32_t mouseDx;
    int32_t mouseDy;
    int32_t rawWeaponSelect;
    uint8_t leftHandDesired;
    CSGOUserCmdPB inputHistory;
};

// ---- recording ----
bool StartRecord(int slot); // clears old buffer, begins capture
bool StopRecord(int slot); // stops
bool IsRecording(int slot);
// Checks for active recordings.
bool HasAnyRecording();
// Ends runtime state and retains recordings.
void InvalidateSlot(int slot);
// Pawn session ID for callbacks and async work.
uint64_t PawnGeneration(int slot);
int RecordedTickCount(int slot); // <0 on bad slot
int RecordedSubtickCount(int slot); // <0 on bad slot
int RecordedCommandCount(int slot); // <0 on bad slot
// Serialized history size in bytes.
int RecordedInputHistorySize(int slot);

// Captures state before queued client commands.
bool OnRecordCommandPre(int slot, void* services);
// Commits one completed command.
void OnRecordCommandPost(int slot, void* services);
// Stashes the current command's subtick moves.
void OnCaptureSubticks(int slot, const SubtickMove* moves, int count);
// Stores command input and optional history.
void OnCaptureCommand(int slot, const ReplayCommandFrameData& command, std::string history);
// Tracks the recording's weapon services.
void SetLiveWs(int slot, void* ws);
// Tracks SelectItem's weapon definition.
void SetCurrentDef(int slot, int defIndex);

// Copies recorded data; returns the count written.
int CopyTicks(int slot, ReplayTick* out, int maxTicks);
int CopyTicksRange(int slot, int start, ReplayTick* out, int maxTicks);
int CopySubticks(int slot, SubtickMove* out, int maxSubticks);
int CopySubticksRange(int slot, int start, SubtickMove* out, int maxSubticks);
int CopyCommands(int slot, ReplayCommandFrameData* out, int maxCommands);
int CopyCommandsRange(int slot, int start, ReplayCommandFrameData* out, int maxCommands);
// Copies a bounded recorded history range.
int CopyInputHistoryRange(int slot, int start, uint8_t* out, int maxBytes);

// ---- replay ----
// Loads aligned replay buffers.
bool LoadReplay(int slot,
                const ReplayTick* ticks,
                int tickCount,
                const SubtickMove* subs,
                int subCount,
                const ReplayCommandFrameData* commands,
                int commandCount,
                const uint8_t* inputHistory = nullptr,
                int inputHistorySize = 0,
                uint64_t pawnGeneration = 0) noexcept;
bool StartReplay(int slot, bool loop); // play from tick 0
bool StopReplay(int slot); // stop + clear injection
bool IsReplaying(int slot);
// Checks for active replays.
bool HasAnyReplay();
int ReplayCursor(int slot); // current tick index, <0 if idle
int ReplayTotal(int slot); // loaded tick count

// Last completed frame; starts with frame 0.
bool CurrentReplayTick(int slot, ReplayTick& out);
// Assembles input for the next native command.
bool ReplayCommandFrameForSimulation(int slot, void* services, ReplayCommandFrame& out);
// Current command's view snapshot.
bool ReplayCommandViewSnapshot(int slot, MovementSnapshot& out);

// Switches the live Pawn's weapon.
bool SwitchWeaponByDef(int slot, int defIndex);

// Active Bot/player weapon definition.
int ActiveWeaponDefForSlot(int slot);

// Matches CT and T fire grenades.
bool ReplayWeaponDefsMatch(int firstDef, int secondDef);

// Consumes the current tick's drop event once
bool TakeCurrentReplayDrop(int slot, ReplayDropEvent& event);
// Drops the recorded item from the registered Pawn.
bool DropReplayEventWeapon(int slot, void* services, const ReplayDropEvent& event);

// Checks drop-hook availability.
bool DropHookReady();
// Installs drop release-pose hooks.
bool InstallDropReleasePose(void* outerDrop, void* buildTransform);

// ---- replay write hooks ----
// Restores state before weapon and grenade processing.
bool OnReplayCommandPre(int slot, void* services, const ReplayTick& tick);
// Restores end state and advances one command.
void OnReplayCommandPost(int slot, void* services);

void ClearAll(); // wipe all record + replay buffers
} // namespace motion_recorder
} // namespace cs2bc
