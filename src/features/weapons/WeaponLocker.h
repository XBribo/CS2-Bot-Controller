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

// Select the current pawn's locked weapon, Bot or player.
// Returns: 0 ok / 1 no ws / 2 no target / 3 unavailable / 4 replay active.
int SwitchToLockTarget(int slot);

// ---- shared inventory operations ----

// True once GetSlot + SelectItem are resolved and hooks installed
bool WeaponHooksReady();

// Read a weapon's Schema-backed item definition. -1 if unresolved.
int ReadDefIndex(void* weapon);

// Entity index of a weapon (identity ehandle low bits). -1 if null
int WeaponEntIndex(void* weapon);

// Resolves a command's entity index only within the owner's inventory; normalizes knives.
int WeaponDefForEntityIndex(void* ws, int entityIndex);

// Active weapon's def index for a WeaponServices*. -1 if none/unresolved
int ActiveWeaponDef(void* ws);

// First weapon in slots 0..4 whose def index == def
void* FindWeaponByDef(void* ws, int def);

// Request a switch through SelectItem, respecting all registered hooks.
bool SelectWeapon(void* ws, void* weapon);

// Bypass SelectItem hooks only for internal replay operations.
bool SelectWeaponRaw(void* ws, void* weapon);

} // namespace weapon_locker_hooks
} // namespace cs2bc
