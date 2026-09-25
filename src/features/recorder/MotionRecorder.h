// Motion recording & replay

#pragma once

#include <cstdint>

namespace cs2bc {
// State of the player at one boundary of a movement tick. Captured twice
// per tick: pre (before the mover runs) and post (after).
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

// One recorded server tick. numSubtick subtick moves follow this tick in
// the parallel SubtickMove buffer
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

struct ReplayMovementExtra
{
    uint32_t fields;
    float jumpPressedTime;
    float lastDuckTime;
    int32_t lastActualJumpPressTick;
    float lastActualJumpPressFrac;
    int32_t lastUsableJumpPressTick;
    float lastUsableJumpPressFrac;
    int32_t lastLandedTick;
    float lastLandedFrac;
    float lastLandedVelocityX;
    float lastLandedVelocityY;
    float lastLandedVelocityZ;
};
#pragma pack(pop)

static_assert(sizeof(ReplayCommandFrameData) == 68);
static_assert(sizeof(ReplayMovementExtra) == 48);
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

// Complete replay input frame assembled for PlayerRunCommand
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
};

// ---- recording ----
bool StartRecord(int slot); // clears old buffer, begins capture
bool StopRecord(int slot); // stops
bool IsRecording(int slot);
// Reports whether any slot has an active recording.
bool HasAnyRecording();
int RecordedTickCount(int slot); // <0 on bad slot
int RecordedSubtickCount(int slot); // <0 on bad slot
int RecordedCommandCount(int slot); // <0 on bad slot

// PhysicsSimulate hook: capture pre snapshot
void OnCapturePre(int slot, void* services, void* cmd);
// PhysicsSimulate hook: capture post snapshot + commit the tick
void OnCapturePost(int slot, void* services, void* cmd);
// PlayerRunCommand hook: stash this tick's subtick moves
void OnCaptureSubticks(int slot, const SubtickMove* moves, int count);
// PlayerRunCommand hook: stash this tick's complete command frame
void OnCaptureCommand(int slot, const ReplayCommandFrameData& command);
// Track which WeaponServices* maps to this recording slot
void SetLiveWs(int slot, void* ws);
void* LiveWs(int slot);
// SelectItem tap: update the slot's current weapon def index.
void SetCurrentDef(int slot, int defIndex);

// Copy recorded data out to caller buffers; returns elements written.
int CopyTicks(int slot, ReplayTick* out, int maxTicks);
int CopySubticks(int slot, SubtickMove* out, int maxSubticks);
int CopyCommands(int slot, ReplayCommandFrameData* out, int maxCommands);

// ---- replay ----
// Load all parallel replay arrays into a slot's replay buffer
bool LoadReplay(int slot,
                const ReplayTick* ticks,
                int tickCount,
                const SubtickMove* subs,
                int subCount,
                const ReplayCommandFrameData* commands,
                int commandCount,
                const ReplayMovementExtra* movementExtras,
                int movementExtraCount) noexcept;
bool StartReplay(int slot, bool loop); // play from tick 0
bool StopReplay(int slot); // stop + clear injection
bool IsReplaying(int slot);
// Reports whether any slot has an active replay.
bool HasAnyReplay();
int ReplayCursor(int slot); // current tick index, <0 if idle
int ReplayTotal(int slot); // loaded tick count

// Current tick being applied this server tick
bool CurrentReplayTick(int slot, ReplayTick& out);
// Assemble all replay input fields for the next simulated tick
bool ReplayCommandFrameForSimulation(int slot, ReplayCommandFrame& out);
// Command view angles for the tick currently being simulated.
bool ReplayCommandViewSnapshot(int slot, MovementSnapshot& out);
// Copy the current tick's subtick moves into out
// Returns count, or -1 if not replaying.
int CurrentReplaySubticks(int slot, SubtickMove* out, int maxOut);

// Buttons of the tick about to be simulated
bool CurrentReplayInputButtons(int slot, uint64_t& b0, uint64_t& b1, uint64_t& b2);

// Switch a bot to the weapon with this def index.
bool SwitchBotWeaponByDef(int slot, int defIndex);

// Def index of the weapon the bot currently holds.
int BotActiveWeaponDef(int slot);

// Treats CT and T fire grenades as the same replay weapon type
bool ReplayWeaponDefsMatch(int firstDef, int secondDef);

// Entity index to write into cmd.weaponselect this replay tick
int CurrentReplayWeaponSelect(int slot);
int CurrentReplayWeaponDef(int slot);
// Consumes the current tick's drop event once
bool TakeCurrentReplayDrop(int slot, ReplayDropEvent& event);
// Drops the recorded item through the bot pawn's native weapon service
bool DropReplayEventWeapon(int slot, void* services, const ReplayDropEvent& event);

// Reports whether the weapon drop hook is available.
bool DropHookReady();
// Installs the Windows release-pose capture and replay hooks.
bool InstallDropReleasePose(void* outerDrop, void* buildTransform);

// ---- replay write hooks ----
// PlayerRunCommand (pre): seed pawn state consumed by weapon and grenade logic
void OnReplayCommandPre(int slot, void* services, const ReplayTick& tick);
// PhysicsSimulate (post): restore the end snapshot and advance the cursor.
void OnReplayCommit(int slot, void* services, bool simulated);

void ClearAll(); // wipe all record + replay buffers
} // namespace motion_recorder
} // namespace cs2bc
