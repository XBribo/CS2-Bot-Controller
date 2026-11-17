// Bot profile access by slot.

#pragma once

#include <cstdint>

namespace cs2bc {
// Fixed BotProfile layout for C ABI and C#.
#pragma pack(push, 4)
struct BotProfileData
{
    float aggression; // 0..1
    float skill; // 0..1
    float teamwork; // 0..1
    float reactionTime; // seconds
    float attackDelay; // seconds
    float lookAccelAtk; // m_lookAngleMaxAccelAttacking
    float lookStiffAtk; // m_lookAngleStiffnessAttacking
    float lookDampAtk; // m_lookAngleDampingAttacking
    int32_t cost; // m_cost
    int32_t difficulty; // m_difficultyFlags (bitmask)
    int32_t weaponPrefCount; // valid entries in weaponPref
    uint16_t weaponPref[16]; // item def index, [0..weaponPrefCount)
};
enum BotProfileFields : uint32_t
{
    Skill = 1U << 0,
    ReactionTime = 1U << 1,
    AttackDelay = 1U << 2,
    LookAccelNormal = 1U << 3,
    LookStiffNormal = 1U << 4,
    LookDampNormal = 1U << 5,
    LookAccelAtk = 1U << 6,
    LookStiffAtk = 1U << 7,
    LookDampAtk = 1U << 8,
    Aggression = 1U << 9,
    Teamwork = 1U << 10,
    AimFocusInitial = 1U << 11,
    AimFocusDecay = 1U << 12,
    AimFocusOffsetScale = 1U << 13,
    AimFocusInterval = 1U << 14,
    VoicePitch = 1U << 15,
};

struct BotProfilePatch
{
    uint32_t fields;
    float skill;
    float reactionTime;
    float attackDelay;
    float lookAccelNormal;
    float lookStiffNormal;
    float lookDampNormal;
    float lookAccelAtk;
    float lookStiffAtk;
    float lookDampAtk;
    float aggression;
    float teamwork;
    float aimFocusInitial;
    float aimFocusDecay;
    float aimFocusOffsetScale;
    float aimFocusInterval;
    int32_t voicePitch;
};
static_assert(sizeof(BotProfilePatch) == 68);
#pragma pack(pop)

namespace bot_profile {
// Reads the current Bot profile; false if unavailable.
bool ReadProfile(int slot, BotProfileData& out);
int ReadProfileName(int slot, char* buffer, int capacity);
bool SetProfile(int slot, const BotProfilePatch& patch);
} // namespace bot_profile
} // namespace cs2bc
