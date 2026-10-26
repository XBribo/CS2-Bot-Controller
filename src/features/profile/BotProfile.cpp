// Bot profile access by slot.

#include "BotProfile.h"
#include "ccsbot_slot.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "offsets.h"
#include "plugin.h"

#include <engine2/isource2server.h>
#include <game/entitynetworkable.h>
#include <entityhandle.h>

#include <algorithm>
#include <cstring>

namespace tg = cs2bc::offsets;

namespace cs2bc {
namespace bot_profile {
// Resolves and reads the current Bot profile.
bool ReadProfile(int slot, BotProfileData& out)
{
    if (slot < 0 || slot >= 64 || !g_SMAPI) return false;
    auto* server = static_cast<ISource2Server*>(g_SMAPI->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr));
    if (!server) return false;

    Entity2Networkable_t controller{};
    if (!server->GetEntity2Networkable(CEntityIndex(slot + 1), &controller) || !controller.m_pEntityInstance) return false;
    uint32_t pawnHandle = 0;
    const int controllerPawn = cs2_sdk::schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
    if (!GuardedRead(controller.m_pEntityInstance, controllerPawn, pawnHandle) || pawnHandle == 0 || pawnHandle == 0xFFFFFFFFU)
        return false;

    Entity2Networkable_t pawn{};
    if (!server->GetEntity2Networkable(CEntityIndex(CEntityHandle(pawnHandle).GetEntryIndex()), &pawn) || !pawn.m_pEntity ||
        !pawn.m_pEntityInstance || pawn.m_pEntity->GetRefEHandle().ToInt() != pawnHandle || !pawn.m_pszCPPClassname ||
        std::strcmp(pawn.m_pszCPPClassname, "CCSPlayerPawn") != 0)
        return false;

    void* bot = nullptr;
    const int pawnBot = cs2_sdk::schema::GetFieldOffset("CCSPlayerPawn", "m_pBot");
    if (!GuardedRead(pawn.m_pEntityInstance, pawnBot, bot) || !bot) return false;
    const auto resolved = ResolveSlot(bot);
    if (resolved.slot != slot || resolved.pawn != pawn.m_pEntityInstance) return false;

    void* prof = nullptr;
    if (!GuardedRead(bot, tg::g_botProfile, prof)) return false;
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
    count = std::max(count, 0);
    count = std::min(count, 16);
    out.weaponPrefCount = count;
    for (int i = 0; i < count; ++i)
        if (!SafeRead(prof, tg::g_profWeaponPref + (i * 2), out.weaponPref[i])) return false;
    return true;
}
} // namespace bot_profile
} // namespace cs2bc
