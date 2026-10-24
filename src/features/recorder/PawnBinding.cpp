#include "PawnBinding.h"
#include "InputInjector.h"
#include "MotionRecorder.h"
#include "ccsbot_slot.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/log.h"
#include "offsets.h"
#include "plugin.h"

#include <engine2/isource2server.h>
#include <game/entitynetworkable.h>
#include <entityhandle.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>

namespace tg = cs2bc::offsets;

namespace cs2bc::input_injector {
namespace {
struct Binding
{
    void* pawn = nullptr;
    void* services = nullptr;
    uint32_t pawnHandle = 0;
    uint32_t controllerHandle = 0;
};
std::array<Binding, kMaxSlots> g_bindings{};
std::array<std::mutex, kMaxSlots> g_bindingMutexes;
std::atomic<uint64_t> g_registeredSlots{ 0 };
ISource2Server* g_server = nullptr;
int g_controllerPawn = -1;

// Checks the fixed slot table.
bool ValidSlot(int slot) { return slot >= 0 && slot < kMaxSlots; }

// Resolves both entities before touching their services.
Binding CurrentBinding(int slot)
{
    if (!ValidSlot(slot) || !g_server) return {};
    Entity2Networkable_t controller{};
    if (!g_server->GetEntity2Networkable(CEntityIndex(slot + 1), &controller) || !controller.m_pEntity ||
        !controller.m_pEntityInstance)
        return {};
    uint32_t pawnHandle = 0;
    if (!GuardedRead(controller.m_pEntityInstance, g_controllerPawn, pawnHandle) || pawnHandle == 0 || pawnHandle == 0xFFFFFFFFU)
        return {};
    Entity2Networkable_t pawn{};
    if (!g_server->GetEntity2Networkable(CEntityIndex(CEntityHandle(pawnHandle).GetEntryIndex()), &pawn) || !pawn.m_pEntity ||
        !pawn.m_pEntityInstance || pawn.m_pEntity->GetRefEHandle().ToInt() != pawnHandle || !pawn.m_pszCPPClassname ||
        std::strcmp(pawn.m_pszCPPClassname, "CCSPlayerPawn") != 0)
        return {};
    const uint32_t controllerHandle = controller.m_pEntity->GetRefEHandle().ToInt();
    const auto handles = ReadPawnControllerHandles(pawn.m_pEntityInstance);
    const uint32_t owner = handles.controllerSlot >= 0 ? handles.controllerHandle : handles.originalControllerHandle;
    if (handles.ownerSlot != slot || owner != controllerHandle) return {};
    void* services = nullptr;
    void* fieldPawn = nullptr;
    if (!GuardedRead(pawn.m_pEntityInstance, tg::g_pawnMovementServices, services) || !services ||
        !GuardedRead(services, tg::g_servicesPawn, fieldPawn) || fieldPawn != pawn.m_pEntityInstance)
        return {};
    return { pawn.m_pEntityInstance, services, pawnHandle, controllerHandle };
}

// Invalidates stale registrations instead of following a replacement pawn.
Binding RegisteredBinding(int slot)
{
    if (!ValidSlot(slot)) return {};
    Binding registered;
    {
        std::scoped_lock lock(g_bindingMutexes[slot]);
        registered = g_bindings[slot];
    }
    if (!registered.pawn) return {};
    const Binding current = CurrentBinding(slot);
    if (current.pawn == registered.pawn && current.services == registered.services && current.pawnHandle == registered.pawnHandle &&
        current.controllerHandle == registered.controllerHandle)
        return registered;
    motion_recorder::InvalidateSlot(slot);
    BC_LOG_WARN("slot %d: pawn changed; stopped", slot);
    return {};
}
} // namespace

// Registers only the controller's current pawn.
bool SetReplayPawn(int slot, void* pawn)
{
    if (!ValidSlot(slot) || !pawn) return false;
    const Binding registered = RegisteredBinding(slot);
    if (registered.pawn == pawn) return true;
    if (motion_recorder::IsRecording(slot) || motion_recorder::IsReplaying(slot)) return false;
    const Binding current = CurrentBinding(slot);
    if (current.pawn != pawn) return false;
    motion_recorder::InvalidateSlot(slot);
    {
        std::scoped_lock lock(g_bindingMutexes[slot]);
        g_bindings[slot] = current;
    }
    g_registeredSlots.fetch_or(uint64_t{ 1 } << slot, std::memory_order_release);
    return true;
}

// Detaches both pawn and services without touching stored recordings.
void ClearReplayPawn(int slot)
{
    if (!ValidSlot(slot)) return;
    std::scoped_lock lock(g_bindingMutexes[slot]);
    g_bindings[slot] = {};
    g_registeredSlots.fetch_and(~(uint64_t{ 1 } << slot), std::memory_order_release);
}

// Never falls back to an unregistered pawn.
void* ResolveReplayPawn(int slot, void* services)
{
    const Binding binding = RegisteredBinding(slot);
    return services && binding.services == services ? binding.pawn : nullptr;
}

namespace pawn_binding {
// Caches the engine interface and schema field once.
bool Init()
{
    g_server = static_cast<ISource2Server*>(g_SMAPI->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr));
    g_controllerPawn = cs2_sdk::schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
    return g_server && g_controllerPawn >= 0;
}

// Returns only the registered, live services.
void* ServicesForSlot(int slot) { return RegisteredBinding(slot).services; }

// New input requests explicitly bind the current pawn.
bool BindCurrentPawn(int slot)
{
    if (RegisteredBinding(slot).pawn) return true;
    return SetReplayPawn(slot, CurrentBinding(slot).pawn);
}

// Resolves the callback owner without adopting its pawn.
int ServicesToSlot(void* services, void** validatedPawn)
{
    void* pawn = nullptr;
    if (services) GuardedRead(services, tg::g_servicesPawn, pawn);
    if (validatedPawn) *validatedPawn = pawn;
    return ControllerSlotForPawn(pawn);
}

// Uses the same registration as the enclosing movement callback.
void* ServicesToWeaponServices(int slot, void* services, void* validatedPawn)
{
    void* pawn = ResolveReplayPawn(slot, services);
    if (!pawn || pawn != validatedPawn) return nullptr;
    void* weapons = nullptr;
    return GuardedRead(pawn, tg::g_pawnWeaponServices, weapons) ? weapons : nullptr;
}

// Checks only registered slots, once per provider tick.
void Refresh()
{
    uint64_t slots = g_registeredSlots.load(std::memory_order_acquire);
    while (slots)
    {
        RegisteredBinding(std::countr_zero(slots));
        slots &= slots - 1;
    }
}

// Ends sessions on map changes and provider unload.
void Reset()
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
        motion_recorder::InvalidateSlot(slot);
}

// Clears registrations after callbacks have drained.
void ClearAll()
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
        ClearReplayPawn(slot);
    g_server = nullptr;
    g_controllerPawn = -1;
}
} // namespace pawn_binding
} // namespace cs2bc::input_injector
