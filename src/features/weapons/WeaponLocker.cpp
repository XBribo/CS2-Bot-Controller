#include "core/gameconfig.h"
// Detours for CCSBot::EquipBestWeapon, EquipPistol, and
// CCSPlayer_WeaponServices::SelectItem

#include "WeaponLocker.h"
#include "nlohmann/json.hpp"
#include "core/memory_module.h"
#include "WeaponLockerState.h"
#include "ccsbot_slot.h"
#include "MotionRecorder.h"
#include "offsets.h"
#include "hooks.h"
#include "plugin.h"
#include "core/cs2_sdk/schema.h"
#include <engine2/isource2server.h>
#include <game/entitynetworkable.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>

namespace tg = cs2bc::offsets;

using GetSlotT = void*(BC_FASTCALL*)(void* ws, int slot, unsigned int mask);
using SelectItemT = char(BC_FASTCALL*)(void* ws, void* weapon, int flag);

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
ISource2Server* g_server = nullptr;
int g_controllerPawn = -1;
int g_pawnBot = -1;
int g_lifeState = -1;

struct LiveWeaponServices
{
    void* pawn = nullptr;
    void* ws = nullptr;
    uint32_t pawnHandle = 0;
};

// Resolve both ends from the current entity table before using a native service.
LiveWeaponServices ResolveLiveWs(int slot)
{
    if (!g_server || slot < 0 || slot >= 64) return {};
    Entity2Networkable_t controller{};
    if (!g_server->GetEntity2Networkable(CEntityIndex(slot + 1), &controller) || !controller.m_pEntity ||
        !controller.m_pEntityInstance) return {};
    uint32_t pawnHandle = 0;
    if (!GuardedRead(controller.m_pEntityInstance, g_controllerPawn, pawnHandle) ||
        pawnHandle == 0 || pawnHandle == 0xFFFFFFFFU) return {};
    Entity2Networkable_t pawn{};
    if (!g_server->GetEntity2Networkable(CEntityIndex(pawnHandle & 0x7FFFU), &pawn) || !pawn.m_pEntity ||
        !pawn.m_pEntityInstance || pawn.m_pEntity->GetRefEHandle().ToInt() != pawnHandle ||
        !pawn.m_pszCPPClassname || std::strcmp(pawn.m_pszCPPClassname, "CCSPlayerPawn") != 0) return {};
    const auto handles = ReadPawnControllerHandles(pawn.m_pEntityInstance);
    if (handles.controllerHandle != controller.m_pEntity->GetRefEHandle().ToInt() ||
        (handles.originalControllerIndex > 0 && handles.originalControllerHandle != handles.controllerHandle)) return {};
    uint8_t lifeState = 0xFF;
    void* bot = nullptr;
    void* ws = nullptr;
    if (!GuardedRead(pawn.m_pEntityInstance, g_lifeState, lifeState) || lifeState != 0 ||
        !GuardedRead(pawn.m_pEntityInstance, g_pawnBot, bot) || !bot ||
        !GuardedRead(pawn.m_pEntityInstance, tg::g_pawnWeaponServices, ws) || !ws) return {};
    return { .pawn = pawn.m_pEntityInstance, .ws = ws, .pawnHandle = pawnHandle };
}

// Cache only enough identity to find a slot; native calls revalidate it.
struct WsBinding
{
    int slot;
    uint32_t pawnHandle;
};
std::unordered_map<void*, WsBinding> g_wsToBinding; // NOLINT(bugprone-throwing-static-initialization)
// Inverse: slot -> WeaponServices*
void* g_slotToWs[64] = { nullptr };
std::mutex g_wsToSlotMu;

// Reuses the pawn already validated by the enclosing equipment hook.
void RememberWsForPawn(void* pawn, int slot)
{
    if (!pawn || slot < 0 || slot >= 64) return;
    const auto live = ResolveLiveWs(slot);
    if (live.pawn != pawn || !live.ws) return;
    std::scoped_lock lk(g_wsToSlotMu);
    auto old = g_wsToBinding.find(g_slotToWs[slot]);
    if (old != g_wsToBinding.end() && old->second.slot == slot) g_wsToBinding.erase(old);
    g_wsToBinding[live.ws] = { .slot = slot, .pawnHandle = live.pawnHandle };
    g_slotToWs[slot] = live.ws;
}

WsBinding LookupBindingForWs(void* ws)
{
    if (!ws) return { .slot = -1, .pawnHandle = 0 };
    std::scoped_lock lk(g_wsToSlotMu);
    auto it = g_wsToBinding.find(ws);
    return it == g_wsToBinding.end() ? WsBinding{ .slot = -1, .pawnHandle = 0 } : it->second;
}

// LockTarget -> engine weapon-slot index
int LockTargetToEngineSlot(LockTarget t)
{
    int v = static_cast<int>(t);
    if (v < 1 || v > 5) return -1;
    return v - 1;
}

bool IsGrenadeDef(int def) { return def >= 43 && def <= 48; }

// ---- detours ----

// Blocks automatic weapon selection while a lock or replay owns it.
KHook::Return<void> HookedEquipBestWeapon(void* bot, char mustEquip) noexcept
{
    auto sr = ResolveSlot(bot);
    if (sr.slot >= 0) RememberWsForPawn(sr.pawn, sr.slot);
    if (sr.slot >= 0 && motion_recorder::IsReplaying(sr.slot)) return { KHook::Action::Supersede };
    LockTarget lt = (sr.slot >= 0) ? weapon_locker_state::Get(sr.slot) : LockTarget::None;
    if (lt != LockTarget::None) return { KHook::Action::Supersede };
    return { KHook::Action::Ignore };
}

// Applies the same ownership rule to pistol selection.
KHook::Return<void> HookedEquipPistol(void* bot, char mustEquip) noexcept
{
    auto sr = ResolveSlot(bot);
    if (sr.slot >= 0) RememberWsForPawn(sr.pawn, sr.slot);
    if (sr.slot >= 0 && motion_recorder::IsReplaying(sr.slot)) return { KHook::Action::Supersede };
    LockTarget lt = (sr.slot >= 0) ? weapon_locker_state::Get(sr.slot) : LockTarget::None;
    if (lt != LockTarget::None) return { KHook::Action::Supersede };
    return { KHook::Action::Ignore };
}

// Records weapon changes and rejects switches away from the locked slot.
KHook::Return<char> HookedSelectItem(void* ws, void* weapon, int flag) noexcept
{
    // Recording : a human switching weapons calls SelectItem
    if (weapon && motion_recorder::HasAnyRecording())
    {
        int def = ReadDefIndex(weapon);
        if (def >= 0)
            for (int s = 0; s < motion_recorder::kMaxSlots; ++s)
            {
                if (motion_recorder::IsRecording(s) && motion_recorder::LiveWs(s) == ws) motion_recorder::SetCurrentDef(s, def);
            }
    }

    WsBinding bind = LookupBindingForWs(ws);
    if (bind.slot < 0) return { KHook::Action::Ignore };

    const auto live = ResolveLiveWs(bind.slot);
    if (live.ws != ws || live.pawnHandle != bind.pawnHandle) return { KHook::Action::Ignore };

    if (motion_recorder::IsReplaying(bind.slot)) return { KHook::Action::Ignore };

    LockTarget lt = weapon_locker_state::Get(bind.slot);
    if (lt == LockTarget::None) return { KHook::Action::Ignore };

    int engineSlot = LockTargetToEngineSlot(lt);
    if (engineSlot < 0 || !g_getSlot) return { KHook::Action::Ignore };

    if (engineSlot == 3 && weapon && IsGrenadeDef(ReadDefIndex(weapon))) return { KHook::Action::Ignore };

    void* targetWeapon = g_getSlot(ws, engineSlot, 0xFFFFFFFFU);
    // No weapon in the locked slot -> can't enforce, let it through.
    if (!targetWeapon) return { KHook::Action::Ignore };

    // Switch is to the lock target -> allow.
    if (weapon == targetWeapon) return { KHook::Action::Ignore };

    // Switch is to something else -> block.
    return { KHook::Action::Supersede, 0 };
}

// ---- install / remove ----

} // namespace

bool Install(const nlohmann::json& gd, const modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen)
{
    g_server = static_cast<ISource2Server*>(g_SMAPI->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr));
    g_controllerPawn = schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
    g_pawnBot = schema::GetFieldOffset("CCSPlayerPawn", "m_pBot");
    g_lifeState = schema::GetFieldOffset("CBaseEntity", "m_lifeState");
    if (!g_server || g_controllerPawn < 0 || g_pawnBot < 0 || g_lifeState < 0)
    {
        g_status = "failed: live weapon services resolution";
        if (errorOut && errorOutLen) std::snprintf(errorOut, errorOutLen, "%s", g_status.c_str());
        return false;
    }
    g_addrEquipBestWeapon = gameconfig::ResolveSig(gd, serverModule, "CCSBot::EquipBestWeapon", errorOut, errorOutLen);
    if (!g_addrEquipBestWeapon)
    {
        g_status = "failed: EquipBestWeapon sig";
        return false;
    }

    g_addrEquipPistol = gameconfig::ResolveSig(gd, serverModule, "CCSBot::EquipPistol", errorOut, errorOutLen);
    if (!g_addrEquipPistol)
    {
        g_status = "failed: EquipPistol sig";
        return false;
    }

    g_addrSelectItem = gameconfig::ResolveSig(gd, serverModule, "CCSPlayer_WeaponServices::SelectItem", errorOut, errorOutLen);
    if (!g_addrSelectItem)
    {
        g_status = "failed: SelectItem sig";
        return false;
    }

    g_addrGetSlot = gameconfig::ResolveSig(gd, serverModule, "CCSPlayer_WeaponServices::GetSlot", errorOut, errorOutLen);
    if (!g_addrGetSlot)
    {
        g_status = "failed: GetSlot sig";
        return false;
    }
    g_getSlot = reinterpret_cast<GetSlotT>(g_addrGetSlot);

    auto failCleanup = [&](const char* what) -> bool {
        std::snprintf(errorOut, errorOutLen, "%s failed", what);
        g_hookEquipBestWeapon.Remove();
        g_hookEquipPistol.Remove();
        g_hookSelectItem.Remove();
        return false;
    };

    if (!g_hookEquipBestWeapon.Install(g_addrEquipBestWeapon, &HookedEquipBestWeapon))
    {
        g_status = "failed: Create EquipBestWeapon";
        return failCleanup("Create EquipBestWeapon");
    }

    if (!g_hookEquipPistol.Install(g_addrEquipPistol, &HookedEquipPistol))
    {
        g_status = "failed: Create EquipPistol";
        return failCleanup("Create EquipPistol");
    }

    if (!g_hookSelectItem.Install(g_addrSelectItem, &HookedSelectItem))
    {
        g_status = "failed: Create SelectItem";
        return failCleanup("Create SelectItem");
    }

    g_installed = true;
    g_status = "ok";
    return true;
}

void Remove()
{
    g_server = nullptr;
    if (!g_installed) return;
    g_hookSelectItem.Remove();
    g_hookEquipPistol.Remove();
    g_hookEquipBestWeapon.Remove();
    g_installed = false;
    g_status = "not_attempted";
    {
        std::scoped_lock lk(g_wsToSlotMu);
        g_wsToBinding.clear();
        for (auto& slotToWs : g_slotToWs)
            slotToWs = nullptr;
    }
}

const char* Status() { return g_status.c_str(); }
void* EquipBestWeaponAddress() { return g_addrEquipBestWeapon; }
void* EquipPistolAddress() { return g_addrEquipPistol; }
void* SelectItemAddress() { return g_addrSelectItem; }
void* GetSlotAddress() { return g_addrGetSlot; }

// ---- MotionRecorder helpers ----

bool WeaponHooksReady() { return g_installed && g_getSlot && g_hookSelectItem.Active(); }

int ReadDefIndex(void* weapon)
{
    if (!weapon) return -1;
    uint16_t def = 0;
    return SafeRead(weapon, tg::g_weaponItemDefIndex, def) ? def : -1;
}

// entity -> identity(0x10) -> m_EHandle(0x10), low 15 bits = index.
namespace {

int EntIndexOf(void* entity)
{
    if (!entity) return -1;
    void* identity = nullptr;
    if (!GuardedRead(entity, tg::g_entIdentity, identity)) return -1;
    if (!identity) return -1;
    uint32_t h = 0;
    if (!SafeRead(identity, tg::g_entIdentityEHandle, h)) return -1;
    if (h == 0U || h == 0xFFFFFFFFU) return -1;
    return static_cast<int>(h & 0x7FFFU);
}

} // namespace

// entity index of a weapon, for cmd.weaponselect on replay.
int WeaponEntIndex(void* weapon) { return EntIndexOf(weapon); }

// Records the requested item, which may differ from the active item during a throw.
int WeaponDefForEntityIndex(void* ws, int entityIndex)
{
    if (!ws || !g_getSlot || entityIndex <= 0 || entityIndex >= 0x7FFF) return -1;
    for (int slot = 0; slot <= 4; ++slot)
    {
        const unsigned int count = slot == 3 ? 8U : 1U;
        for (unsigned int pos = 0; pos < count; ++pos)
        {
            void* weapon = g_getSlot(ws, slot, slot == 3 ? pos : 0xFFFFFFFFU);
            if (EntIndexOf(weapon) != entityIndex) continue;
            const int def = ReadDefIndex(weapon);
            return slot == 2 && def >= 0 && def != 31 ? kKnifeDef : def;
        }
    }
    return -1;
}

// Stops the inventory search as soon as the active item is found.
int ActiveWeaponDef(void* ws)
{
    uint32_t handle = 0;
    if (!ws || !SafeRead(ws, tg::g_wsActiveWeapon, handle) || handle == 0U || handle == 0xFFFFFFFFU) return -1;
    return WeaponDefForEntityIndex(ws, static_cast<int>(handle & 0x7FFFU));
}

void* FindWeaponByDef(void* ws, int def)
{
    if (!ws || def < 0 || !g_getSlot) return nullptr;
    // kKnifeDef means "the bot's own slot-2 knife", whatever skin it is.
    if (def == kKnifeDef) return g_getSlot(ws, 2, 0xFFFFFFFFU);
    // Non-grenade gear slots hold one weapon each
    for (int slot = 0; slot <= 4; ++slot)
    {
        if (slot == 3) continue;
        void* w = g_getSlot(ws, slot, 0xFFFFFFFFU);
        if (w && ReadDefIndex(w) == def) return w;
    }
    // GEAR_SLOT_GRENADES (3) holds every grenade type at once
    for (unsigned int pos = 0; pos < 8; ++pos)
    {
        void* w = g_getSlot(ws, 3, pos);
        if (w && ReadDefIndex(w) == def) return w;
    }
    return nullptr;
}

bool SelectWeapon(void* ws, void* weapon)
{
    if (!ws || !weapon || !g_installed || !g_addrSelectItem) return false;
    // Ordinary requests must respect all registered weapon-selection hooks.
    return reinterpret_cast<SelectItemT>(g_addrSelectItem)(ws, weapon, 0) != 0;
}

bool SelectWeaponRaw(void* ws, void* weapon)
{
    if (!ws || !weapon || !g_hookSelectItem.Active()) return false;
    g_hookSelectItem.CallOriginal(ws, weapon, 0);
    return true;
}

void* WsForSlot(int slot)
{
    return ResolveLiveWs(slot).ws;
}

int SwitchToLockTarget(int slot)
{
    if (!g_installed || !g_hookSelectItem.Active() || !g_getSlot) return 3;
    if (slot < 0 || slot >= 64) return 3;
    if (motion_recorder::IsReplaying(slot)) return 4;

    LockTarget lt = weapon_locker_state::Get(slot);
    if (lt == LockTarget::None) return 3;
    int engineSlot = LockTargetToEngineSlot(lt);
    if (engineSlot < 0) return 3;

    void* ws = WsForSlot(slot);
    if (!ws) return 1;

    void* target = g_getSlot(ws, engineSlot, 0xFFFFFFFFU);
    if (!target) return 2;

    SelectWeapon(ws, target);
    return 0;
}
} // namespace weapon_locker_hooks
} // namespace cs2bc
