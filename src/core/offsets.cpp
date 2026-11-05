#include "core/cs2_sdk/src/core/gameconfig.h"
#include "offsets.h"
#include "nlohmann/json.hpp"
#include "core/cs2_sdk/src/core/schema.h"

#include <cstdint>
#include <cstdio>

namespace cs2bc::offsets {
bool LoadFromGamedata(const nlohmann::json& gamedata, char* error, size_t maxlen)
{
    const cs2_sdk::gameconfig::OffsetBinding offsets[] = {
        { &g_vtIdxTeleport, "CBaseEntity_Teleport" },
        { &g_botProfile, "CCSBot::Profile" },
        { &g_profAggression, "BotProfile::Aggression" },
        { &g_profSkill, "BotProfile::Skill" },
        { &g_profTeamwork, "BotProfile::Teamwork" },
        { &g_profWeaponPref, "BotProfile::WeaponPref" },
        { &g_profWeaponPrefCount, "BotProfile::WeaponPrefCount" },
        { &g_profCost, "BotProfile::Cost" },
        { &g_profDifficulty, "BotProfile::Difficulty" },
        { &g_profReactionTime, "BotProfile::ReactionTime" },
        { &g_profAttackDelay, "BotProfile::AttackDelay" },
        { &g_profLookAccelAtk, "BotProfile::LookAngleMaxAccelAttacking" },
        { &g_profLookStiffAtk, "BotProfile::LookAngleStiffnessAttacking" },
        { &g_profLookDampAtk, "BotProfile::LookAngleDampingAttacking" },
        { &g_buyInitialDelay, "BuyState::InitialDelay" },
        { &g_buyDoneBuying, "BuyState::DoneBuying" },
        { &g_entIdentityEHandle, "CEntityIdentity::EHandle" },
        { &g_servicesPawn, "CCSPlayer_MovementServices::Pawn" },
        { &g_vtIdxPlayerRunCommand, "vtidx::PlayerRunCommand" },
        { &g_vtIdxControllerCommandSetup, "vtidx::ControllerCommandSetup" },
        { &g_vtIdxDropWeapon, "vtidx::DropWeapon" },
    };
    for (const auto& offset : offsets)
    {
        *offset.target = cs2_sdk::gameconfig::FindPlatformOffset(gamedata, offset.name, -1);
        if (*offset.target < 0)
        {
            std::snprintf(error, maxlen, "Invalid gamedata offset '%s.offsets.%s': expected a nonnegative int", offset.name,
                          cs2_sdk::gameconfig::PlatformName());
            return false;
        }
    }
    return true;
}

bool LoadFromSchema(char* error, size_t maxlen)
{
    const cs2_sdk::schema::FieldBinding fields[] = {
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

    if (!cs2_sdk::schema::ResolveFields(fields, error, maxlen)) return false;

    g_nodeAbsRotation = cs2_sdk::schema::GetFieldOffset("CGameSceneNode", "m_angAbsRotation");

    const cs2_sdk::schema::Field itemFields[] = {
        { "CEconEntity", "m_AttributeManager" },
        { "CAttributeContainer", "m_Item" },
        { "CEconItemView", "m_iItemDefinitionIndex" },
    };
    g_weaponItemDefIndex = cs2_sdk::schema::GetEmbeddedFieldOffset(itemFields, error, maxlen);
    if (g_weaponItemDefIndex < 0) return false;

    const cs2_sdk::schema::Field buttonFields[] = {
        { "CPlayer_MovementServices", "m_nButtons" },
        { "CInButtonState", "m_pButtonStates" },
    };
    g_servicesButtons = cs2_sdk::schema::GetEmbeddedFieldOffset(buttonFields, error, maxlen);
    if (g_servicesButtons < 0) return false;
    g_servicesButtons1 = g_servicesButtons + static_cast<int>(sizeof(uint64_t));
    g_servicesButtons2 = g_servicesButtons1 + static_cast<int>(sizeof(uint64_t));

    return true;
}
} // namespace cs2bc::offsets
