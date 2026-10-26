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
#pragma pack(pop)

namespace bot_profile {
// Reads the current Bot profile; false if unavailable.
bool ReadProfile(int slot, BotProfileData& out);
} // namespace bot_profile
} // namespace cs2bc
