// Detours for CCSBot::EquipBestWeapon, EquipPistol, and
// CCSPlayer_WeaponServices::SelectItem

#include "WeaponLocker.h"
#include "nlohmann/json.hpp"
#include "sig_scan.h"
#include "WeaponLockerState.h"
#include "ccsbot_slot.h"
#include "MotionRecorder.h"
#include "version_targets.h"
#include "hooks.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <mutex>
#include <unordered_map>

namespace tg = cs2bc::targets;

using GetSlotT = void*(BC_FASTCALL*)(void* ws, int slot, unsigned int mask);

namespace cs2bc {
namespace weapon_locker_hooks {

namespace {

GetSlotT g_getSlot = nullptr;

void* g_addrEquipBestWeapon = nullptr;
void* g_addrEquipPistol = nullptr;
void* g_addrSelectItem = nullptr;
void* g_addrGetSlot = nullptr;

hooks::NativeHook<void, void*, char> g_hookEquipBestWeapon;

hooks::NativeHook<void, void*, char> g_hookEquipPistol;

hooks::NativeHook<char, void*, void*, int> g_hookSelectItem;

std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

bool g_installed = false;

// -----------------------------------------------------------------------------
// WeaponServices cache
//
// These are live game-object pointers and MUST NOT survive runtime Disable().
// -----------------------------------------------------------------------------

struct WsBinding
{
    int slot;
    void* pawn;
};

// WeaponServices* -> (bot slot, pawn)
std::unordered_map<void*, WsBinding> g_wsToBinding; // NOLINT(bugprone-throwing-static-initialization)

// slot -> WeaponServices*
void* g_slotToWs[64] = { nullptr };

std::mutex g_wsToSlotMu;

// -----------------------------------------------------------------------------
// Clear cached live WeaponServices / pawn pointers.
//
// Safe to call repeatedly.
// -----------------------------------------------------------------------------

void ClearWeaponServiceBindings()
{
    std::scoped_lock lk(g_wsToSlotMu);

    g_wsToBinding.clear();

    for (auto& ws : g_slotToWs)
        ws = nullptr;
}

// -----------------------------------------------------------------------------
// Stores WeaponServices for a bot slot.
// -----------------------------------------------------------------------------

void RememberWsForBot(void* bot, int slot)
{
    if (!bot || slot < 0 || slot >= 64)
    {
        return;
    }

    void* pawn = nullptr;

    if (!GuardedRead(bot, tg::g_botPawn, pawn))
    {
        return;
    }

    if (!pawn) return;

    void* ws = nullptr;

    if (!GuardedRead(pawn, tg::g_pawnWeaponServices, ws))
    {
        return;
    }

    if (!ws) return;

    std::scoped_lock lk(g_wsToSlotMu);

    g_wsToBinding[ws] = { .slot = slot, .pawn = pawn };

    g_slotToWs[slot] = ws;
}

// -----------------------------------------------------------------------------
// Finds cached slot/pawn information for WeaponServices.
// -----------------------------------------------------------------------------

WsBinding LookupBindingForWs(void* ws)
{
    if (!ws)
    {
        return { .slot = -1, .pawn = nullptr };
    }

    std::scoped_lock lk(g_wsToSlotMu);

    auto it = g_wsToBinding.find(ws);

    if (it == g_wsToBinding.end())
    {
        return { .slot = -1, .pawn = nullptr };
    }

    return it->second;
}

// -----------------------------------------------------------------------------
// LockTarget -> engine weapon-slot index.
// -----------------------------------------------------------------------------

int LockTargetToEngineSlot(LockTarget target)
{
    const int value = static_cast<int>(target);

    if (value < 1 || value > 5)
    {
        return -1;
    }

    return value - 1;
}

bool IsGrenadeDef(int def) { return def >= 43 && def <= 48; }

// -----------------------------------------------------------------------------
// Detours
// -----------------------------------------------------------------------------

// Blocks automatic weapon selection while a lock or replay owns it.
KHook::Return<void> HookedEquipBestWeapon(void* bot, char /*mustEquip*/) noexcept
{
    auto sr = ResolveSlot(bot);

    if (sr.slot >= 0)
    {
        RememberWsForBot(bot, sr.slot);
    }

    if (sr.slot >= 0 && motion_recorder::IsReplaying(sr.slot))
    {
        return { KHook::Action::Supersede };
    }

    const LockTarget target = (sr.slot >= 0) ? weapon_locker_state::Get(sr.slot) : LockTarget::None;

    if (target != LockTarget::None)
    {
        return { KHook::Action::Supersede };
    }

    return { KHook::Action::Ignore };
}

// Applies the same ownership rule to pistol selection.
KHook::Return<void> HookedEquipPistol(void* bot, char /*mustEquip*/) noexcept
{
    auto sr = ResolveSlot(bot);

    if (sr.slot >= 0)
    {
        RememberWsForBot(bot, sr.slot);
    }

    if (sr.slot >= 0 && motion_recorder::IsReplaying(sr.slot))
    {
        return { KHook::Action::Supersede };
    }

    const LockTarget target = (sr.slot >= 0) ? weapon_locker_state::Get(sr.slot) : LockTarget::None;

    if (target != LockTarget::None)
    {
        return { KHook::Action::Supersede };
    }

    return { KHook::Action::Ignore };
}

// Records weapon changes and rejects switches away from the locked slot.
KHook::Return<char> HookedSelectItem(void* ws, void* weapon, int /*flag*/) noexcept
{
    // Recording:
    //
    // Humans switching weapons also call SelectItem.
    if (weapon)
    {
        const int def = ReadDefIndex(weapon);

        if (def >= 0)
        {
            for (int slot = 0; slot < motion_recorder::kMaxSlots; ++slot)
            {
                if (motion_recorder::IsRecording(slot) && motion_recorder::LiveWs(slot) == ws)
                {
                    motion_recorder::SetCurrentDef(slot, def);
                }
            }
        }
    }

    const WsBinding binding = LookupBindingForWs(ws);

    if (binding.slot < 0)
    {
        return { KHook::Action::Ignore };
    }

    // Human took over this pawn:
    //
    // current m_hController no longer belongs to the bot slot that was
    // originally cached. Never block the human's weapon changes.
    const int currentSlot = ControllerSlotForPawn(binding.pawn);

    if (currentSlot != binding.slot)
    {
        return { KHook::Action::Ignore };
    }

    if (motion_recorder::IsReplaying(binding.slot))
    {
        return { KHook::Action::Ignore };
    }

    const LockTarget target = weapon_locker_state::Get(binding.slot);

    if (target == LockTarget::None)
    {
        return { KHook::Action::Ignore };
    }

    const int engineSlot = LockTargetToEngineSlot(target);

    if (engineSlot < 0 || !g_getSlot)
    {
        return { KHook::Action::Ignore };
    }

    // Grenade slot contains multiple distinct weapons.
    if (engineSlot == 3 && weapon && IsGrenadeDef(ReadDefIndex(weapon)))
    {
        return { KHook::Action::Ignore };
    }

    void* targetWeapon = g_getSlot(ws, engineSlot, 0xFFFFFFFFU);

    // No weapon in the locked slot -> cannot enforce the lock.
    if (!targetWeapon)
    {
        return { KHook::Action::Ignore };
    }

    // Switching to the lock target is allowed.
    if (weapon == targetWeapon)
    {
        return { KHook::Action::Ignore };
    }

    // Any other switch is blocked.
    return { KHook::Action::Supersede, 0 };
}

} // namespace

// -----------------------------------------------------------------------------
// Install
//
// IMPORTANT:
//
// This function is intentionally idempotent.
//
// Install(); Install(); must leave exactly one hook set installed.
// -----------------------------------------------------------------------------

bool Install(const nlohmann::json& gd, const sig::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen)
{
    // Critical protection against double installation.
    if (g_installed)
    {
        g_status = "ok";
        return true;
    }

    // Defensive cleanup.
    //
    // If a previous installation failed halfway through, ensure that no
    // partial hooks, cached pointers or GetSlot function pointer remain.
    Remove();

    // Clear previously resolved addresses before resolving a new set.
    g_addrEquipBestWeapon = nullptr;
    g_addrEquipPistol = nullptr;
    g_addrSelectItem = nullptr;
    g_addrGetSlot = nullptr;

    // ---------------------------------------------------------------------
    // CCSBot::EquipBestWeapon
    // ---------------------------------------------------------------------

    g_addrEquipBestWeapon = sig::ResolveSig(gd, serverModule, "CCSBot::EquipBestWeapon", errorOut, errorOutLen);

    if (!g_addrEquipBestWeapon)
    {
        Remove();

        g_status = "failed: EquipBestWeapon sig";

        return false;
    }

    // ---------------------------------------------------------------------
    // CCSBot::EquipPistol
    // ---------------------------------------------------------------------

    g_addrEquipPistol = sig::ResolveSig(gd, serverModule, "CCSBot::EquipPistol", errorOut, errorOutLen);

    if (!g_addrEquipPistol)
    {
        Remove();

        g_status = "failed: EquipPistol sig";

        return false;
    }

    // ---------------------------------------------------------------------
    // CCSPlayer_WeaponServices::SelectItem
    // ---------------------------------------------------------------------

    g_addrSelectItem = sig::ResolveSig(gd, serverModule, "CCSPlayer_WeaponServices::SelectItem", errorOut, errorOutLen);

    if (!g_addrSelectItem)
    {
        Remove();

        g_status = "failed: SelectItem sig";

        return false;
    }

    // ---------------------------------------------------------------------
    // CCSPlayer_WeaponServices::GetSlot
    //
    // This one is not hooked; it is used as the original engine helper for
    // weapon lookup.
    // ---------------------------------------------------------------------

    g_addrGetSlot = sig::ResolveSig(gd, serverModule, "CCSPlayer_WeaponServices::GetSlot", errorOut, errorOutLen);

    if (!g_addrGetSlot)
    {
        Remove();

        g_status = "failed: GetSlot sig";

        return false;
    }

    g_getSlot = reinterpret_cast<GetSlotT>(g_addrGetSlot);

    // ---------------------------------------------------------------------
    // Small common rollback helper.
    // ---------------------------------------------------------------------

    auto failCleanup = [&](const char* errorMessage, const char* status) -> bool {
        // Remove everything that might already have been installed.
        Remove();

        if (errorOut && errorOutLen > 0)
        {
            std::snprintf(errorOut, errorOutLen, "%s", errorMessage);
        }

        // Set failure status AFTER Remove(), because Remove resets it.
        g_status = status;

        return false;
    };

    // ---------------------------------------------------------------------
    // EquipBestWeapon hook
    // ---------------------------------------------------------------------

    if (!g_hookEquipBestWeapon.Install(g_addrEquipBestWeapon, &HookedEquipBestWeapon))
    {
        return failCleanup("Create EquipBestWeapon failed", "failed: Create EquipBestWeapon");
    }

    // ---------------------------------------------------------------------
    // EquipPistol hook
    // ---------------------------------------------------------------------

    if (!g_hookEquipPistol.Install(g_addrEquipPistol, &HookedEquipPistol))
    {
        return failCleanup("Create EquipPistol failed", "failed: Create EquipPistol");
    }

    // ---------------------------------------------------------------------
    // SelectItem hook
    // ---------------------------------------------------------------------

    if (!g_hookSelectItem.Install(g_addrSelectItem, &HookedSelectItem))
    {
        return failCleanup("Create SelectItem failed", "failed: Create SelectItem");
    }

    g_installed = true;
    g_status = "ok";

    return true;
}

// -----------------------------------------------------------------------------
// Remove
//
// Idempotent:
//
// Remove(); Remove(); Remove();
//
// is always safe.
//
// We deliberately do NOT check g_installed here because this function must
// also clean up partially-created hooks after a failed Install().
// -----------------------------------------------------------------------------

void Remove()
{
    // Reverse installation order.
    g_hookSelectItem.Remove();
    g_hookEquipPistol.Remove();
    g_hookEquipBestWeapon.Remove();

    // Live game-object pointers cannot survive a runtime pause.
    ClearWeaponServiceBindings();

    // GetSlot is runtime-dependent from the point of view of this subsystem.
    // Nulling it also prevents helper methods from touching engine state while
    // WeaponLocker is disabled.
    g_getSlot = nullptr;

    g_installed = false;
    g_status = "not_attempted";
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

const char* Status() { return g_status.c_str(); }

void* EquipBestWeaponAddress() { return g_addrEquipBestWeapon; }

void* EquipPistolAddress() { return g_addrEquipPistol; }

void* SelectItemAddress() { return g_addrSelectItem; }

void* GetSlotAddress() { return g_addrGetSlot; }

// -----------------------------------------------------------------------------
// MotionRecorder helpers
// -----------------------------------------------------------------------------

bool WeaponHooksReady() { return g_installed && g_getSlot && g_hookSelectItem.Active(); }

int ReadDefIndex(void* weapon)
{
    if (!weapon) return -1;

    uint16_t def = 0;

    return SafeRead(weapon, tg::g_weaponItemDefIndex, def) ? static_cast<int>(def) : -1;
}

// -----------------------------------------------------------------------------
// Entity helper
// -----------------------------------------------------------------------------

namespace {

int EntIndexOf(void* entity)
{
    if (!entity) return -1;

    void* identity = nullptr;

    if (!GuardedRead(entity, tg::g_entIdentity, identity))
    {
        return -1;
    }

    if (!identity) return -1;

    uint32_t handle = 0;

    if (!SafeRead(identity, tg::g_entIdentityEHandle, handle))
    {
        return -1;
    }

    if (handle == 0U || handle == 0xFFFFFFFFU)
    {
        return -1;
    }

    return static_cast<int>(handle & 0x7FFFU);
}

} // namespace

// -----------------------------------------------------------------------------
// Entity index of a weapon.
//
// Used for cmd.weaponselect during replay.
// -----------------------------------------------------------------------------

int WeaponEntIndex(void* weapon) { return EntIndexOf(weapon); }

// -----------------------------------------------------------------------------
// Active weapon definition index.
// -----------------------------------------------------------------------------

int ActiveWeaponDef(void* ws)
{
    if (!ws || !g_getSlot || !g_installed)
    {
        return -1;
    }

    // m_hActiveWeapon is a handle.
    //
    // Resolve it by matching its entity index against pointers returned by
    // GetSlot().
    uint32_t activeHandle = 0;

    if (!SafeRead(ws, tg::g_wsActiveWeapon, activeHandle))
    {
        return -1;
    }

    if (activeHandle == 0U || activeHandle == 0xFFFFFFFFU)
    {
        return -1;
    }

    const int activeIndex = static_cast<int>(activeHandle & 0x7FFFU);

    for (int slot = 0; slot <= 4; ++slot)
    {
        // GEAR_SLOT_GRENADES contains several grenades simultaneously.
        const unsigned int maxPos = (slot == 3) ? 8U : 1U;

        for (unsigned int pos = 0; pos < maxPos; ++pos)
        {
            const unsigned int posArg = (slot == 3) ? pos : 0xFFFFFFFFU;

            void* weapon = g_getSlot(ws, slot, posArg);

            if (!weapon || EntIndexOf(weapon) != activeIndex)
            {
                continue;
            }

            const int def = ReadDefIndex(weapon);

            // Engine slot 2 contains both knife and taser.
            //
            // Normalize all knife skins to kKnifeDef, while keeping taser 31
            // distinct.
            if (slot == 2 && def != 31)
            {
                return kKnifeDef;
            }

            return def;
        }
    }

    return -1;
}

// -----------------------------------------------------------------------------
// Finds a weapon by definition index.
// -----------------------------------------------------------------------------

void* FindWeaponByDef(void* ws, int def)
{
    if (!ws || def < 0 || !g_getSlot || !g_installed)
    {
        return nullptr;
    }

    // kKnifeDef means:
    //
    // "the bot's own slot-2 knife", regardless of skin definition.
    if (def == kKnifeDef)
    {
        return g_getSlot(ws, 2, 0xFFFFFFFFU);
    }

    // Non-grenade slots hold one weapon each.
    for (int slot = 0; slot <= 4; ++slot)
    {
        if (slot == 3) continue;

        void* weapon = g_getSlot(ws, slot, 0xFFFFFFFFU);

        if (weapon && ReadDefIndex(weapon) == def)
        {
            return weapon;
        }
    }

    // Grenade slot may contain multiple grenade types.
    for (unsigned int pos = 0; pos < 8; ++pos)
    {
        void* weapon = g_getSlot(ws, 3, pos);

        if (weapon && ReadDefIndex(weapon) == def)
        {
            return weapon;
        }
    }

    return nullptr;
}

// -----------------------------------------------------------------------------
// Switches weapon through the original SelectItem implementation.
// -----------------------------------------------------------------------------

bool SelectWeaponRaw(void* ws, void* weapon)
{
    if (!g_installed || !ws || !weapon || !g_hookSelectItem.Active())
    {
        return false;
    }

    g_hookSelectItem.CallOriginal(ws, weapon, 0);

    return true;
}

// -----------------------------------------------------------------------------
// Cached WeaponServices for a bot slot.
// -----------------------------------------------------------------------------

void* WsForSlot(int slot)
{
    if (!g_installed || slot < 0 || slot >= 64)
    {
        return nullptr;
    }

    std::scoped_lock lk(g_wsToSlotMu);

    return g_slotToWs[slot];
}

// -----------------------------------------------------------------------------
// Forces a bot to its lock target.
//
// Return values:
//   0 - switched successfully
//   1 - WeaponServices unavailable
//   2 - target weapon unavailable
//   3 - runtime/hooks/lock unavailable
//   4 - replay owns weapon state
// -----------------------------------------------------------------------------

int SwitchToLockTarget(int slot)
{
    if (!g_installed || !g_hookSelectItem.Active() || !g_getSlot)
    {
        return 3;
    }

    if (slot < 0 || slot >= 64)
    {
        return 3;
    }

    if (motion_recorder::IsReplaying(slot))
    {
        return 4;
    }

    const LockTarget target = weapon_locker_state::Get(slot);

    if (target == LockTarget::None) return 3;

    const int engineSlot = LockTargetToEngineSlot(target);

    if (engineSlot < 0) return 3;

    void* ws = nullptr;

    {
        std::scoped_lock lk(g_wsToSlotMu);

        ws = g_slotToWs[slot];
    }

    // Bot has not ticked yet.
    //
    // The lock itself remains stored and takes effect once AI runs.
    if (!ws) return 1;

    void* targetWeapon = g_getSlot(ws, engineSlot, 0xFFFFFFFFU);

    if (!targetWeapon) return 2;

    // Call the original implementation directly so we do not recurse through
    // HookedSelectItem.
    g_hookSelectItem.CallOriginal(ws, targetWeapon, 0);

    return 0;
}

} // namespace weapon_locker_hooks
} // namespace cs2bc
