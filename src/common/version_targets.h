// version_targets.h

#pragma once

#include <nlohmann/json.hpp>

namespace BotController::targets
{
    // ---- CCSBot ----

    // AI-ran-this-tick byte flag; set to 1 to fake a completed tick
    inline int kBot_AiTickedFlag = 0x610;
    // CCSBot -> pawn (CCSPlayerPawn*)
    inline int kBot_Pawn = 0x18;
    // CCSBot -> m_profile (BotProfile*)
    inline int kBot_Profile = 0x08;

    // ---- BotProfile (CCSBot+kBot_Profile) ----

    inline int kProf_Aggression = 0x08;      // float, 0..1
    inline int kProf_Skill = 0x0C;           // float, 0..1
    inline int kProf_Teamwork = 0x10;        // float, 0..1
    inline int kProf_WeaponPref = 0x24;      // WORD[16] item def index, stride 2
    inline int kProf_WeaponPrefCount = 0x44; // int
    inline int kProf_Cost = 0x48;            // int
    inline int kProf_Difficulty = 0x50;      // u8 bitflags EASY/NORMAL/HARD/EXPERT
    inline int kProf_ReactionTime = 0x58;    // float
    inline int kProf_AttackDelay = 0x5C;     // float
    inline int kProf_LookAccelAtk = 0x78;    // float m_lookAngleMaxAccelAttacking
    inline int kProf_LookStiffAtk = 0x7C;    // float m_lookAngleStiffnessAttacking
    inline int kProf_LookDampAtk = 0x80;     // float m_lookAngleDampingAttacking

    // ---- BuyState ----

    // m_isInitialDelay (bool)
    inline int kBuy_InitialDelay = 0x08;
    // m_doneBuying (bool)
    inline int kBuy_DoneBuying = 0x18;

    // ---- CBaseEntity / CEntityIdentity ----

    // entity -> CEntityIdentity*
    inline int kEnt_Identity = 0x10;
    // CEntityIdentity -> m_EHandle (low 15 bits = entity index)
    inline int kEntIdentity_EHandle = 0x10;
    // m_MoveType (MoveType_t, 1 byte) — restored each replay tick
    inline int kEnt_MoveType = 0x2F3;
    // m_nActualMoveType (MoveType_t, 1 byte) — networked move type
    inline int kEnt_ActualMoveType = 0x2F5;
    // m_fFlags (bit0 = FL_ONGROUND, bit1 = FL_DUCKING)
    inline int kEnt_Flags = 0x388;
    // m_fFlags bit masks restored on replay (constants, not offsets)
    inline constexpr unsigned kFL_OnGround = 1u << 0;
    inline constexpr unsigned kFL_Ducking = 1u << 1;
    // m_vecAbsVelocity
    inline int kEnt_AbsVelocity = 0x38C;
    // entity -> m_CBodyComponent -> m_pSceneNode
    inline int kEnt_BodyComponent = 0x30;
    inline int kBody_SceneNode = 0x08;
    inline int kNode_AbsOrigin = 0xC8;

    // ---- CCSPlayerPawn ----

    // m_pWeaponServices
    inline int kPawn_WeaponServices = 0xA30;
    // m_pMovementServices
    inline int kPawn_MovementServices = 0xA70;
    // m_hController (CHandle)
    inline int kPawn_Controller = 0xBB0;
    // m_hOriginalController (CHandle)
    inline int kPawn_OriginalController = 0xD24;
    // CCSPlayerPawn -> v_angle (QAngle)
    inline int kPawn_ViewAngle = 0xAE8;
    // CCSPlayerPawn -> v_anglePrevious (QAngle)
    inline int kPawn_ViewAnglePrevious = 0xAF4;
    // Embedded server view-angle change vector
    inline int kPawn_ServerViewAngleChanges = 0xA80;
    // m_angEyeAngles (QAngle) — written each replay tick alongside v_angle
    inline int kPawn_EyeAngles = 0x1368;

    // ---- CCSPlayer_WeaponServices ----

    // m_hActiveWeapon (CHandle)
    inline int kWs_ActiveWeapon = 0x60;

    // ---- CBasePlayerWeapon ----

    // m_AttributeManager -> m_Item -> m_iItemDefinitionIndex,
    inline int kWeapon_ItemDefIndex = 0x978 + 0x50 + 0x38;

    // ---- CCSPlayer_MovementServices ----

    // m_pawn (CCSPlayerPawn*)
    inline int kServices_Pawn = 56;
    // m_nButtons.m_pButtonStates[0..2] — engine button state block (CInButtonState)
    inline int kServices_Buttons = 88;       // states[0] (pressed)
    inline int kServices_Buttons1 = 88 + 8;  // states[1]
    inline int kServices_Buttons2 = 88 + 16; // states[2]

    // duck/ladder state
    inline int kServices_LadderNormal = 0x3F8; // Vector m_vecLadderNormal
    inline int kServices_Ducked = 0x408;       // bool m_bDucked
    inline int kServices_DuckAmount = 0x40C;   // float m_flDuckAmount
    inline int kServices_DuckSpeed = 0x410;    // float m_flDuckSpeed
    inline int kServices_DesiresDuck = 0x415;  // bool m_bDesiresDuck
    inline int kServices_Ducking = 0x416;      // bool m_bDucking

    // ---- CMoveData  ----

    // m_vecVelocity — the velocity TryPlayerMove integrates into origin
    inline int kMove_Velocity = 56;
    // m_vecAbsOrigin — post-move origin written here before FinishMove commits
    inline int kMove_AbsOrigin = 200;

    // ---- vtable indices (CCSPlayer_MovementServices) ----

    inline int kVtIdx_PlayerRunCommand = 25;
    inline int kVtIdx_FinishMove = 38;

    void LoadFromGamedata(const nlohmann::json &gd);

} // namespace BotController::targets
