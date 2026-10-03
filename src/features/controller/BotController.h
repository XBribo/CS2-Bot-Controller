// MinHook install/remove for CCSBot Update/Upkeep.

#pragma once

#include <string>
#include <cstdint>

#include <nlohmann/json.hpp>
#include "core/memory_module.h"

namespace cs2bc {
#pragma pack(push, 4)
// Native AI snapshot; its serial advances only after an actual AI Update.
struct NativePerceptionState
{
    int32_t valid;
    uint32_t enemyHandle;
    int32_t hasEnemy;
    int32_t enemyVisible;
    int32_t visibleEnemyParts;
    int32_t nearbyEnemyCount;
    int32_t lastEnemyDead;
    float lastSawEnemyTimestamp;
    float firstSawEnemyTimestamp;
    float currentEnemyAcquireTimestamp;
    uint32_t updateSerial;
};
#pragma pack(pop)
static_assert(sizeof(NativePerceptionState) == 44);

namespace bot_controller_hooks {
// Resolve sigs and install detours.
bool Install(const nlohmann::json& gd, const modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove detours.
void Remove();

const char* Status();

void* UpdateAddress();
void* UpkeepAddress();
void* UpdateLookAnglesAddress();

// Reports whether the engine view hooks required by replay are installed.
bool ReplayViewReady();

// Last CCSBot* seen in Update for this slot, or nullptr. Used to read
// the bot's BotProfile by slot.
void* BotForSlot(int slot);

// Queues one native best-weapon selection after AI Update; replay, locks and human takeover reject it.
bool RequestEquipBestWeapon(int slot);

// Requires live perception fields and the hooks preserving replay output ownership.
bool NativePerceptionReady();
// Returns the latest snapshot only for the same live AI-owned pawn instance.
bool GetNativePerceptionState(int slot, NativePerceptionState& out);
// Overrides only the FOV test during replay, preserving engine LOS and smoke checks.
bool SetReplayNativeFovOverride(bool enabled);
} // namespace bot_controller_hooks
} // namespace cs2bc
