// Override structure offsets from gamedata.json (platform-aware)

#include "version_targets.h"
#include "schema_resolver.h"
#include "sig_scan.h"

#include <cstdint>
#include <cstdio>

namespace BotController::targets {
// Each offset: gamedata[name].offsets[platform], else keep code default
void LoadFromGamedata(const nlohmann::json& gd)
{
    kBot_Profile = Sig::FindPlatformOffset(gd, "CCSBot::Profile", kBot_Profile);
    kProf_Aggression = Sig::FindPlatformOffset(gd, "BotProfile::Aggression", kProf_Aggression);
    kProf_Skill = Sig::FindPlatformOffset(gd, "BotProfile::Skill", kProf_Skill);
    kProf_Teamwork = Sig::FindPlatformOffset(gd, "BotProfile::Teamwork", kProf_Teamwork);
    kProf_WeaponPref = Sig::FindPlatformOffset(gd, "BotProfile::WeaponPref", kProf_WeaponPref);
    kProf_WeaponPrefCount = Sig::FindPlatformOffset(gd, "BotProfile::WeaponPrefCount", kProf_WeaponPrefCount);
    kProf_Cost = Sig::FindPlatformOffset(gd, "BotProfile::Cost", kProf_Cost);
    kProf_Difficulty = Sig::FindPlatformOffset(gd, "BotProfile::Difficulty", kProf_Difficulty);
    kProf_ReactionTime = Sig::FindPlatformOffset(gd, "BotProfile::ReactionTime", kProf_ReactionTime);
    kProf_AttackDelay = Sig::FindPlatformOffset(gd, "BotProfile::AttackDelay", kProf_AttackDelay);
    kProf_LookAccelAtk = Sig::FindPlatformOffset(gd, "BotProfile::LookAngleMaxAccelAttacking", kProf_LookAccelAtk);
    kProf_LookStiffAtk = Sig::FindPlatformOffset(gd, "BotProfile::LookAngleStiffnessAttacking", kProf_LookStiffAtk);
    kProf_LookDampAtk = Sig::FindPlatformOffset(gd, "BotProfile::LookAngleDampingAttacking", kProf_LookDampAtk);
    kBuy_InitialDelay = Sig::FindPlatformOffset(gd, "BuyState::InitialDelay", kBuy_InitialDelay);
    kBuy_DoneBuying = Sig::FindPlatformOffset(gd, "BuyState::DoneBuying", kBuy_DoneBuying);
    kEntIdentity_EHandle = Sig::FindPlatformOffset(gd, "CEntityIdentity::EHandle", kEntIdentity_EHandle);
    kServices_Pawn = Sig::FindPlatformOffset(gd, "CCSPlayer_MovementServices::Pawn", kServices_Pawn);
    kMove_Velocity = Sig::FindPlatformOffset(gd, "CMoveData::Velocity", kMove_Velocity);
    kMove_AbsOrigin = Sig::FindPlatformOffset(gd, "CMoveData::AbsOrigin", kMove_AbsOrigin);
    kVtIdx_PlayerRunCommand = Sig::FindPlatformOffset(gd, "vtidx::PlayerRunCommand", kVtIdx_PlayerRunCommand);
    kVtIdx_FinishMove = Sig::FindPlatformOffset(gd, "vtidx::FinishMove", kVtIdx_FinishMove);
    kVtIdx_DropWeapon = Sig::FindPlatformOffset(gd, "vtidx::DropWeapon", kVtIdx_DropWeapon);
}

// Resolves one required Schema field into its runtime target
static bool ResolveRequired(int& target, const char* className, const char* fieldName, char* errorOut, size_t errorOutLen)
{
    const int offset = Schema::GetFieldOffset(className, fieldName);
    if (offset >= 0)
    {
        target = offset;
        return true;
    }

    if (errorOut && errorOutLen > 0)
        std::snprintf(errorOut, errorOutLen, "Required Schema field missing: %s::%s", className, fieldName);
    return false;
}

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
        { &kBot_AiTickedFlag, "CCSBot", "m_bEyeAnglesUnderPathFinderControl" },
        { &kBot_Pawn, "CBot", "m_pPlayer" },
        { &kEnt_Identity, "CEntityInstance", "m_pEntity" },
        { &kEnt_MoveType, "CBaseEntity", "m_MoveType" },
        { &kEnt_ActualMoveType, "CBaseEntity", "m_nActualMoveType" },
        { &kEnt_Flags, "CBaseEntity", "m_fFlags" },
        { &kEnt_AbsVelocity, "CBaseEntity", "m_vecAbsVelocity" },
        { &kEnt_BodyComponent, "CBaseEntity", "m_CBodyComponent" },
        { &kBody_SceneNode, "CBodyComponent", "m_pSceneNode" },
        { &kNode_AbsOrigin, "CGameSceneNode", "m_vecAbsOrigin" },
        { &kPawn_WeaponServices, "CBasePlayerPawn", "m_pWeaponServices" },
        { &kPawn_ItemServices, "CBasePlayerPawn", "m_pItemServices" },
        { &kPawn_MovementServices, "CBasePlayerPawn", "m_pMovementServices" },
        { &kPawn_Controller, "CBasePlayerPawn", "m_hController" },
        { &kPawn_OriginalController, "CCSPlayerPawnBase", "m_hOriginalController" },
        { &kPawn_ViewAngle, "CBasePlayerPawn", "v_angle" },
        { &kPawn_ViewAnglePrevious, "CBasePlayerPawn", "v_anglePrevious" },
        { &kPawn_ServerViewAngleChanges, "CBasePlayerPawn", "m_ServerViewAngleChanges" },
        { &kPawn_EyeAngles, "CCSPlayerPawn", "m_angEyeAngles" },
        { &kWs_ActiveWeapon, "CPlayer_WeaponServices", "m_hActiveWeapon" },
        { &kServices_LadderNormal, "CCSPlayer_MovementServices", "m_vecLadderNormal" },
        { &kServices_Ducked, "CCSPlayer_MovementServices", "m_bDucked" },
        { &kServices_DuckAmount, "CCSPlayer_MovementServices", "m_flDuckAmount" },
        { &kServices_DuckSpeed, "CCSPlayer_MovementServices", "m_flDuckSpeed" },
        { &kServices_DesiresDuck, "CCSPlayer_MovementServices", "m_bDesiresDuck" },
        { &kServices_Ducking, "CCSPlayer_MovementServices", "m_bDucking" },
    };

    for (const RequiredField& field : fields)
    {
        if (!ResolveRequired(*field.target, field.className, field.fieldName, errorOut, errorOutLen)) return false;
    }

    int attributeManager = -1;
    int item = -1;
    int itemDefinitionIndex = -1;
    if (!ResolveRequired(attributeManager, "CEconEntity", "m_AttributeManager", errorOut, errorOutLen) ||
        !ResolveRequired(item, "CAttributeContainer", "m_Item", errorOut, errorOutLen) ||
        !ResolveRequired(itemDefinitionIndex, "CEconItemView", "m_iItemDefinitionIndex", errorOut, errorOutLen))
        return false;
    kWeapon_ItemDefIndex = attributeManager + item + itemDefinitionIndex;

    int buttonState = -1;
    int buttonStates = -1;
    if (!ResolveRequired(buttonState, "CPlayer_MovementServices", "m_nButtons", errorOut, errorOutLen) ||
        !ResolveRequired(buttonStates, "CInButtonState", "m_pButtonStates", errorOut, errorOutLen))
        return false;
    kServices_Buttons = buttonState + buttonStates;
    kServices_Buttons1 = kServices_Buttons + static_cast<int>(sizeof(uint64_t));
    kServices_Buttons2 = kServices_Buttons1 + static_cast<int>(sizeof(uint64_t));
    return true;
}
} // namespace BotController::targets
