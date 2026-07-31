// Read a bot's BotProfile (personality / aim / weapon preference) by slot.

#pragma once

#include <cstdint>

namespace BotController {
// Mirror of the fields read from BotProfile. Fixed layout for C-ABI / C#.
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

namespace BotProfile {
// Read the profile of the bot currently on this slot. Returns false if
// the slot has no live bot or the profile pointer is null.
bool ReadProfile(int slot, BotProfileData& out);
} // namespace BotProfile
} // namespace BotController
