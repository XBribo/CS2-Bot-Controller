// Bot AI selection hooks and shared player weapon helpers.

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include "core/cs2_sdk/src/core/memory_module.h"

namespace cs2bc {
namespace weapon_locker_hooks {
// Sentinel def index meaning any knife
constexpr int kKnifeDef = 9001;

bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

void Remove();

const char* Status();

void* EquipBestWeaponAddress();
void* EquipPistolAddress();
void* SelectItemAddress();
void* GetSlotAddress();

// Selects the Bot/player's locked weapon.
// Returns: 0 ok / 1 no ws / 2 no target / 3 unavailable / 4 replay active.
int SwitchToLockTarget(int slot);

// ---- shared inventory operations ----

// Checks GetSlot and SelectItem hooks.
bool WeaponHooksReady();

// Weapon definition; -1 if unresolved.
int ReadDefIndex(void* weapon);

// Weapon entity index; -1 if unavailable.
int WeaponEntIndex(void* weapon);

// Resolves an owned weapon index; normalizes knives.
int WeaponDefForEntityIndex(void* ws, int entityIndex);

// Active weapon definition; -1 if unavailable.
int ActiveWeaponDef(void* ws);

// Finds the definition in slots 0..4.
void* FindWeaponByDef(void* ws, int def);

// Switches through SelectItem hooks.
bool SelectWeapon(void* ws, void* weapon);

// Replay-only switch bypassing SelectItem hooks.
bool SelectWeaponRaw(void* ws, void* weapon);

} // namespace weapon_locker_hooks
} // namespace cs2bc
