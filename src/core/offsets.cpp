#include "core/gameconfig.h"
// Override structure offsets from gamedata.json

#include "offsets.h"
#include "nlohmann/json.hpp"
#include "core/cs2_sdk/schema.h"
#include "core/memory_module.h"

#include <cstdint>
#include <cstdio>

namespace cs2bc::offsets {
// Each offset: gamedata[name].offsets[platform], else keep code default
void LoadFromGamedata(const nlohmann::json& gd)
{
    g_vtIdxTeleport = gameconfig::FindPlatformOffset(gd, "CBaseEntity_Teleport", g_vtIdxTeleport);
    g_botProfile = gameconfig::FindPlatformOffset(gd, "CCSBot::Profile", g_botProfile);
    g_profAggression = gameconfig::FindPlatformOffset(gd, "BotProfile::Aggression", g_profAggression);
    g_profSkill = gameconfig::FindPlatformOffset(gd, "BotProfile::Skill", g_profSkill);
    g_profTeamwork = gameconfig::FindPlatformOffset(gd, "BotProfile::Teamwork", g_profTeamwork);
    g_profWeaponPref = gameconfig::FindPlatformOffset(gd, "BotProfile::WeaponPref", g_profWeaponPref);
    g_profWeaponPrefCount = gameconfig::FindPlatformOffset(gd, "BotProfile::WeaponPrefCount", g_profWeaponPrefCount);
    g_profCost = gameconfig::FindPlatformOffset(gd, "BotProfile::Cost", g_profCost);
    g_profDifficulty = gameconfig::FindPlatformOffset(gd, "BotProfile::Difficulty", g_profDifficulty);
    g_profReactionTime = gameconfig::FindPlatformOffset(gd, "BotProfile::ReactionTime", g_profReactionTime);
    g_profAttackDelay = gameconfig::FindPlatformOffset(gd, "BotProfile::AttackDelay", g_profAttackDelay);
    g_profLookAccelAtk = gameconfig::FindPlatformOffset(gd, "BotProfile::LookAngleMaxAccelAttacking", g_profLookAccelAtk);
    g_profLookStiffAtk = gameconfig::FindPlatformOffset(gd, "BotProfile::LookAngleStiffnessAttacking", g_profLookStiffAtk);
    g_profLookDampAtk = gameconfig::FindPlatformOffset(gd, "BotProfile::LookAngleDampingAttacking", g_profLookDampAtk);
    g_buyInitialDelay = gameconfig::FindPlatformOffset(gd, "BuyState::InitialDelay", g_buyInitialDelay);
    g_buyDoneBuying = gameconfig::FindPlatformOffset(gd, "BuyState::DoneBuying", g_buyDoneBuying);
    g_entIdentityEHandle = gameconfig::FindPlatformOffset(gd, "CEntityIdentity::EHandle", g_entIdentityEHandle);
    g_servicesPawn = gameconfig::FindPlatformOffset(gd, "CCSPlayer_MovementServices::Pawn", g_servicesPawn);
    g_moveVelocity = gameconfig::FindPlatformOffset(gd, "CMoveData::Velocity", g_moveVelocity);
    g_moveAbsOrigin = gameconfig::FindPlatformOffset(gd, "CMoveData::AbsOrigin", g_moveAbsOrigin);
    g_vtIdxPlayerRunCommand = gameconfig::FindPlatformOffset(gd, "vtidx::PlayerRunCommand", g_vtIdxPlayerRunCommand);
    g_vtIdxFinishMove = gameconfig::FindPlatformOffset(gd, "vtidx::FinishMove", g_vtIdxFinishMove);
    g_vtIdxControllerCommandSetup = gameconfig::FindPlatformOffset(gd, "vtidx::ControllerCommandSetup", g_vtIdxControllerCommandSetup);
    g_vtIdxDropWeapon = gameconfig::FindPlatformOffset(gd, "vtidx::DropWeapon", g_vtIdxDropWeapon);
}

namespace {

// Resolves one required Schema field into its runtime target
bool ResolveRequired(int& target, const char* className, const char* fieldName, char* errorOut, size_t errorOutLen)
{
    const int offset = schema::GetFieldOffset(className, fieldName);
    if (offset >= 0)
    {
        target = offset;
        return true;
    }

    if (errorOut && errorOutLen > 0) std::snprintf(errorOut, errorOutLen, "Required Schema field missing: %s::%s", className, fieldName);
    return false;
}

} // namespace

// Resolves every required Schema-backed target or reports the first failure
bool LoadFromSchema(char* errorOut, size_t errorOutLen)
{
    struct RequiredField
    {
        int* target;
        const char* className;
        const char* fieldName;
    };

    const RequiredField fields[] = {
        { .target = &g_botAiTickedFlag, .className = "CCSBot", .fieldName = "m_bEyeAnglesUnderPathFinderControl" },
        { .target = &g_botPawn, .className = "CBot", .fieldName = "m_pPlayer" },
        { .target = &g_entIdentity, .className = "CEntityInstance", .fieldName = "m_pEntity" },
        { .target = &g_entMoveType, .className = "CBaseEntity", .fieldName = "m_MoveType" },
        { .target = &g_entActualMoveType, .className = "CBaseEntity", .fieldName = "m_nActualMoveType" },
        { .target = &g_entFlags, .className = "CBaseEntity", .fieldName = "m_fFlags" },
        { .target = &g_entAbsVelocity, .className = "CBaseEntity", .fieldName = "m_vecAbsVelocity" },
        { .target = &g_entBodyComponent, .className = "CBaseEntity", .fieldName = "m_CBodyComponent" },
        { .target = &g_bodySceneNode, .className = "CBodyComponent", .fieldName = "m_pSceneNode" },
        { .target = &g_nodeAbsOrigin, .className = "CGameSceneNode", .fieldName = "m_vecAbsOrigin" },
        { .target = &g_pawnWeaponServices, .className = "CBasePlayerPawn", .fieldName = "m_pWeaponServices" },
        { .target = &g_pawnItemServices, .className = "CBasePlayerPawn", .fieldName = "m_pItemServices" },
        { .target = &g_pawnMovementServices, .className = "CBasePlayerPawn", .fieldName = "m_pMovementServices" },
        { .target = &g_pawnController, .className = "CBasePlayerPawn", .fieldName = "m_hController" },
        { .target = &g_pawnOriginalController, .className = "CCSPlayerPawnBase", .fieldName = "m_hOriginalController" },
        { .target = &g_pawnViewAngle, .className = "CBasePlayerPawn", .fieldName = "v_angle" },
        { .target = &g_pawnViewAnglePrevious, .className = "CBasePlayerPawn", .fieldName = "v_anglePrevious" },
        { .target = &g_pawnServerViewAngleChanges, .className = "CBasePlayerPawn", .fieldName = "m_ServerViewAngleChanges" },
        { .target = &g_pawnEyeAngles, .className = "CCSPlayerPawn", .fieldName = "m_angEyeAngles" },
        { .target = &g_wsActiveWeapon, .className = "CPlayer_WeaponServices", .fieldName = "m_hActiveWeapon" },
        { .target = &g_servicesLadderNormal, .className = "CCSPlayer_MovementServices", .fieldName = "m_vecLadderNormal" },
        { .target = &g_servicesOldViewAngles, .className = "CPlayer_MovementServices", .fieldName = "m_vecOldViewAngles" },
        { .target = &g_servicesDucked, .className = "CCSPlayer_MovementServices", .fieldName = "m_bDucked" },
        { .target = &g_servicesDuckAmount, .className = "CCSPlayer_MovementServices", .fieldName = "m_flDuckAmount" },
        { .target = &g_servicesDuckSpeed, .className = "CCSPlayer_MovementServices", .fieldName = "m_flDuckSpeed" },
        { .target = &g_servicesDesiresDuck, .className = "CCSPlayer_MovementServices", .fieldName = "m_bDesiresDuck" },
        { .target = &g_servicesDucking, .className = "CCSPlayer_MovementServices", .fieldName = "m_bDucking" },
    };

    for (const RequiredField& field : fields)
    {
        if (!ResolveRequired(*field.target, field.className, field.fieldName, errorOut, errorOutLen)) return false;
    }

    g_nodeAbsRotation = schema::GetFieldOffset("CGameSceneNode", "m_angAbsRotation");

    int attributeManager = -1;
    int item = -1;
    int itemDefinitionIndex = -1;
    if (!ResolveRequired(attributeManager, "CEconEntity", "m_AttributeManager", errorOut, errorOutLen) ||
        !ResolveRequired(item, "CAttributeContainer", "m_Item", errorOut, errorOutLen) ||
        !ResolveRequired(itemDefinitionIndex, "CEconItemView", "m_iItemDefinitionIndex", errorOut, errorOutLen))
        return false;
    g_weaponItemDefIndex = attributeManager + item + itemDefinitionIndex;

    int buttonState = -1;
    int buttonStates = -1;
    if (!ResolveRequired(buttonState, "CPlayer_MovementServices", "m_nButtons", errorOut, errorOutLen) ||
        !ResolveRequired(buttonStates, "CInButtonState", "m_pButtonStates", errorOut, errorOutLen))
        return false;
    g_servicesButtons = buttonState + buttonStates;
    g_servicesButtons1 = g_servicesButtons + static_cast<int>(sizeof(uint64_t));
    g_servicesButtons2 = g_servicesButtons1 + static_cast<int>(sizeof(uint64_t));

    return true;
}
} // namespace cs2bc::offsets
