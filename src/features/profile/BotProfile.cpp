#include "BotProfile.h"
#include "ccsbot_slot.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "offsets.h"
#include "plugin.h"

#include <engine2/isource2server.h>
#include <tier0/threadtools.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace tg = cs2bc::offsets;

namespace cs2bc {
namespace bot_profile {
namespace {
void* ResolveProfile(int slot, bool requireOwned = false)
{
    if (!g_SMAPI || !ThreadInMainThread()) return nullptr;
    auto* server = static_cast<ISource2Server*>(g_SMAPI->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr));
    const int controllerPawn = cs2_sdk::schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
    const auto live = ResolvePawnForSlot(server, slot, controllerPawn);
    void* bot = nullptr;
    void* botPawn = nullptr;
    const int pawnBot = cs2_sdk::schema::GetFieldOffset("CCSPlayerPawn", "m_pBot");
    if (!GuardedRead(live.pawn, pawnBot, bot) || !GuardedRead(bot, tg::g_botPawn, botPawn) || botPawn != live.pawn)
        return nullptr;

    void* prof = nullptr;
    if (!GuardedRead(bot, tg::g_botProfile, prof) || !prof) return nullptr;
    if (requireOwned)
    {
        void* owned = nullptr;
        if (!GuardedRead(bot, tg::g_botLocalProfile, owned) || owned != prof) return nullptr;
    }
    return prof;
}
} // namespace

bool ReadProfile(int slot, BotProfileData& out)
{
    const void* prof = ResolveProfile(slot);
    if (!prof) return false;

    std::memset(&out, 0, sizeof(out));
    if (!SafeRead(prof, tg::g_profAggression, out.aggression) || !SafeRead(prof, tg::g_profSkill, out.skill) ||
        !SafeRead(prof, tg::g_profTeamwork, out.teamwork) || !SafeRead(prof, tg::g_profReactionTime, out.reactionTime) ||
        !SafeRead(prof, tg::g_profAttackDelay, out.attackDelay) || !SafeRead(prof, tg::g_profLookAccelAtk, out.lookAccelAtk) ||
        !SafeRead(prof, tg::g_profLookStiffAtk, out.lookStiffAtk) || !SafeRead(prof, tg::g_profLookDampAtk, out.lookDampAtk) ||
        !SafeRead(prof, tg::g_profCost, out.cost) || !SafeRead(prof, tg::g_profDifficulty, out.difficulty))
        return false;

    int count = 0;
    if (!SafeRead(prof, tg::g_profWeaponPrefCount, count)) return false;
    count = std::clamp(count, 0, static_cast<int>(std::size(out.weaponPref)));
    out.weaponPrefCount = count;
    for (int i = 0; i < count; ++i)
        if (!SafeRead(prof, tg::g_profWeaponPref + (i * 2), out.weaponPref[i])) return false;
    return true;
}

int ReadProfileName(int slot, char* buffer, int capacity)
{
    if (!buffer || capacity <= 0) return -1;
    const void* prof = ResolveProfile(slot);
    const char* name = nullptr;
    if (!prof || !GuardedRead(prof, 0, name) || !name) return -1;

    for (int i = 0; i < capacity; ++i)
    {
        if (!GuardedRead(name, i, buffer[i])) return -1;
        if (buffer[i] == '\0') return i;
    }
    return -1;
}

bool SetProfile(int slot, const BotProfilePatch& patch)
{
    if (patch.fields == 0) return false;
    void* prof = ResolveProfile(slot, true);
    if (!prof) return false;

    struct Field
    {
        BotProfileFields flag;
        int offset;
        const float* value;
        float previous = 0.0F;
    };
    Field fields[] = {
        { Skill, tg::g_profSkill, &patch.skill },
        { ReactionTime, tg::g_profReactionTime, &patch.reactionTime },
        { AttackDelay, tg::g_profAttackDelay, &patch.attackDelay },
        { LookAccelNormal, tg::g_profLookAccelNormal, &patch.lookAccelNormal },
        { LookStiffNormal, tg::g_profLookStiffNormal, &patch.lookStiffNormal },
        { LookDampNormal, tg::g_profLookDampNormal, &patch.lookDampNormal },
        { LookAccelAtk, tg::g_profLookAccelAtk, &patch.lookAccelAtk },
        { LookStiffAtk, tg::g_profLookStiffAtk, &patch.lookStiffAtk },
        { LookDampAtk, tg::g_profLookDampAtk, &patch.lookDampAtk },
    };
    uint32_t remaining = patch.fields;
    for (Field& field : fields)
    {
        if ((remaining & field.flag) == 0) continue;
        remaining &= ~field.flag;
        const bool accel = field.flag == LookAccelNormal || field.flag == LookAccelAtk;
        const float value = *field.value;
        constexpr int kLastScalarOffset = 0x80;
        if (field.offset < static_cast<int>(sizeof(void*)) || field.offset > kLastScalarOffset ||
            field.offset % static_cast<int>(alignof(float)) != 0 || !(value >= 0.0F) ||
            (!accel && !std::isfinite(value)) || (field.flag == Skill && value > 1.0F) ||
            !GuardedRead(prof, field.offset, field.previous))
            return false;
    }
    if (remaining != 0) return false;

    for (size_t i = 0; i < std::size(fields); ++i)
    {
        const Field& field = fields[i];
        if ((patch.fields & field.flag) == 0 || TryWriteMemoryGuarded(prof, field.offset, field.value, sizeof(float)))
            continue;

        // Failed writes can be partial.
        for (size_t j = i + 1; j > 0; --j)
        {
            const Field& rollback = fields[j - 1];
            if ((patch.fields & rollback.flag) != 0)
                TryWriteMemoryGuarded(prof, rollback.offset, &rollback.previous, sizeof(float));
        }
        return false;
    }
    return true;
}
} // namespace bot_profile
} // namespace cs2bc
