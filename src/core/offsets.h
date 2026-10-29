#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

namespace cs2bc::offsets {
// Teleport slot for replay positioning.
inline int g_vtIdxTeleport = -1;
// ---- CCSBot ----

// AI tick-complete flag; 1 skips the native tick.
inline int g_botAiTickedFlag = -1;
// CCSBot -> pawn (CCSPlayerPawn*)
inline int g_botPawn = -1;
// CCSBot -> m_profile (BotProfile*)
inline int g_botProfile = -1;

// ---- BotProfile (CCSBot+kBot_Profile) ----

inline int g_profAggression = -1; // float, 0..1
inline int g_profSkill = -1; // float, 0..1
inline int g_profTeamwork = -1; // float, 0..1
inline int g_profWeaponPref = -1; // WORD[16] item def index, stride 2
inline int g_profWeaponPrefCount = -1; // int
inline int g_profCost = -1; // int
inline int g_profDifficulty = -1; // u8 bitflags EASY/NORMAL/HARD/EXPERT
inline int g_profReactionTime = -1; // float
inline int g_profAttackDelay = -1; // float
inline int g_profLookAccelAtk = -1; // float m_lookAngleMaxAccelAttacking
inline int g_profLookStiffAtk = -1; // float m_lookAngleStiffnessAttacking
inline int g_profLookDampAtk = -1; // float m_lookAngleDampingAttacking

// ---- BuyState ----

// m_isInitialDelay (bool)
inline int g_buyInitialDelay = -1;
// m_doneBuying (bool)
inline int g_buyDoneBuying = -1;

// ---- CBaseEntity / CEntityIdentity ----

// entity -> CEntityIdentity*
inline int g_entIdentity = -1;
// CEntityIdentity -> m_EHandle (low 15 bits = entity index)
inline int g_entIdentityEHandle = -1;
// m_MoveType (MoveType_t, 1 byte); restored during replay.
inline int g_entMoveType = -1;
// m_nActualMoveType (MoveType_t, 1 byte); networked.
inline int g_entActualMoveType = -1;
// m_fFlags (bit0 = FL_ONGROUND, bit1 = FL_DUCKING)
inline int g_entFlags = -1;
// Replay-restored m_fFlags bits.
inline constexpr unsigned kFlOnGround = 1U << 0;
inline constexpr unsigned kFlDucking = 1U << 1;
// m_vecAbsVelocity
inline int g_entAbsVelocity = -1;
// entity -> m_CBodyComponent -> m_pSceneNode
inline int g_entBodyComponent = -1;
inline int g_bodySceneNode = -1;
inline int g_nodeAbsOrigin = -1;
inline int g_nodeAbsRotation = -1;

// ---- CCSPlayerPawn ----

// m_pWeaponServices
inline int g_pawnWeaponServices = -1;
// m_pItemServices
inline int g_pawnItemServices = -1;
// m_pMovementServices
inline int g_pawnMovementServices = -1;
// m_hController (CHandle)
inline int g_pawnController = -1;
// m_hOriginalController (CHandle)
inline int g_pawnOriginalController = -1;
// CCSPlayerPawn -> v_angle (QAngle)
inline int g_pawnViewAngle = -1;
// CCSPlayerPawn -> v_anglePrevious (QAngle)
inline int g_pawnViewAnglePrevious = -1;
// m_angEyeAngles (QAngle); restored with v_angle.
inline int g_pawnEyeAngles = -1;

// ---- CCSPlayer_WeaponServices ----

// m_hActiveWeapon (CHandle)
inline int g_wsActiveWeapon = -1;

// ---- CBasePlayerWeapon ----

// m_AttributeManager -> m_Item -> m_iItemDefinitionIndex,
inline int g_weaponItemDefIndex = -1;

// ---- CCSPlayer_MovementServices ----

// m_pawn (CCSPlayerPawn*)
inline int g_servicesPawn = -1;
// m_nButtons.m_pButtonStates[0..2] (CInButtonState).
inline int g_servicesButtons = -1; // final held state
inline int g_servicesButtons1 = -1; // transition count bit 0
inline int g_servicesButtons2 = -1; // transition count bit 1
// Previous command view angles consumed by PlayerRunCommand
inline int g_servicesOldViewAngles = -1;

// duck/ladder state
inline int g_servicesLadderNormal = -1; // Vector m_vecLadderNormal
inline int g_servicesDucked = -1; // bool m_bDucked
inline int g_servicesDuckAmount = -1; // float m_flDuckAmount
inline int g_servicesDuckSpeed = -1; // float m_flDuckSpeed
inline int g_servicesDesiresDuck = -1; // bool m_bDesiresDuck
inline int g_servicesDucking = -1; // bool m_bDucking

// ---- vtable indices (CCSPlayer_MovementServices) ----

inline int g_vtIdxPlayerRunCommand = -1;
// Controller setup immediately preceding queued client commands.
inline int g_vtIdxControllerCommandSetup = -1;
// CCSPlayer_WeaponServices::DropWeapon
inline int g_vtIdxDropWeapon = -1;

// Loads required platform offsets.
bool LoadFromGamedata(const nlohmann::json& gamedata, char* error, size_t maxlen);

// Resolves required Schema offsets; reports the first failure.
bool LoadFromSchema(char* error, size_t maxlen);

} // namespace cs2bc::offsets
