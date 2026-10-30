#include "PawnBinding.h"
#include "InputInjector.h"
#include "MotionRecorder.h"
#include "ccsbot_slot.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/log.h"
#include "offsets.h"
#include "plugin.h"
#include "dispatch.h"
#include "playercommand.h"

#include <engine2/ivengineserver2.h>
#include <engine2/gameshareddefs.h>
#include <engine2/isource2server.h>
#include <game/entitynetworkable.h>
#include <entityhandle.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
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
    void* controller = nullptr;
};
std::array<Binding, kMaxSlots> g_bindings{};
std::array<std::mutex, kMaxSlots> g_bindingMutexes;
std::atomic<uint64_t> g_registeredSlots{ 0 };
ISource2Server* g_server = nullptr;
int g_controllerPawn = -1;
int g_lifeState = -1;
int g_controllerTickBase = -1;

// Checks the fixed slot table.
bool ValidSlot(int slot) { return slot >= 0 && slot < kMaxSlots; }

// Resolves the live controller, Pawn, and services.
Binding CurrentBinding(int slot, const char** failure = nullptr)
{
    const auto fail = [failure](const char* reason) {
        if (failure) *failure = reason;
        return Binding{};
    };
    if (!ValidSlot(slot) || !g_server) return fail("binding unavailable");
    Entity2Networkable_t controller{};
    if (!g_server->GetEntity2Networkable(CEntityIndex(slot + 1), &controller) || !controller.m_pEntity ||
        !controller.m_pEntityInstance)
        return fail("controller missing");
    uint32_t pawnHandle = 0;
    if (!GuardedRead(controller.m_pEntityInstance, g_controllerPawn, pawnHandle) || pawnHandle == 0 || pawnHandle == 0xFFFFFFFFU)
        return fail("player pawn missing");
    Entity2Networkable_t pawn{};
    if (!g_server->GetEntity2Networkable(CEntityIndex(CEntityHandle(pawnHandle).GetEntryIndex()), &pawn) || !pawn.m_pEntity ||
        !pawn.m_pEntityInstance)
        return fail("pawn entity missing");
    if (pawn.m_pEntity->GetRefEHandle().ToInt() != pawnHandle) return fail("pawn handle mismatch");
    if (!pawn.m_pszCPPClassname || std::strcmp(pawn.m_pszCPPClassname, "CCSPlayerPawn") != 0)
        return fail("pawn class mismatch");
    const uint32_t controllerHandle = controller.m_pEntity->GetRefEHandle().ToInt();
    const auto handles = ReadPawnControllerHandles(pawn.m_pEntityInstance);
    const uint32_t owner = handles.controllerSlot >= 0 ? handles.controllerHandle : handles.originalControllerHandle;
    if (handles.ownerSlot != slot || owner != controllerHandle) return fail("pawn owner mismatch");
    void* services = nullptr;
    void* fieldPawn = nullptr;
    if (!GuardedRead(pawn.m_pEntityInstance, tg::g_pawnMovementServices, services) || !services ||
        !GuardedRead(services, tg::g_servicesPawn, fieldPawn) || fieldPawn != pawn.m_pEntityInstance)
        return fail("movement services mismatch");
    return { pawn.m_pEntityInstance, services, pawnHandle, controllerHandle, controller.m_pEntityInstance };
}

// Invalidates changed Pawn registrations.
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
        current.controllerHandle == registered.controllerHandle && current.controller == registered.controller)
        return registered;
    motion_recorder::InvalidateSlot(slot);
    BC_LOG_WARN("slot %d: pawn changed; stopped", slot);
    return {};
}

// Reads a living Pawn's weapon services.
void* ReadWeaponServices(const Binding& binding)
{
    uint8_t lifeState = 0xFF;
    void* weapons = nullptr;
    void* owner = nullptr;
    if (!binding.pawn || !GuardedRead(binding.pawn, g_lifeState, lifeState) || lifeState != 0 ||
        !GuardedRead(binding.pawn, tg::g_pawnWeaponServices, weapons) || !weapons ||
        !GuardedRead(weapons, tg::g_servicesPawn, owner) || owner != binding.pawn)
        return nullptr;
    return weapons;
}
} // namespace

// Registers only the controller's current pawn.
bool SetReplayPawn(int slot, void* pawn)
{
    if (!ValidSlot(slot) || !pawn)
    {
        BC_LOG_WARN("slot %d: invalid pawn registration", slot);
        return false;
    }
    const Binding registered = RegisteredBinding(slot);
    if (registered.pawn == pawn) return true;
    if (motion_recorder::IsRecording(slot) || motion_recorder::IsReplaying(slot))
    {
        BC_LOG_WARN("slot %d: registration failed (session active)", slot);
        return false;
    }
    const char* failure = "player pawn mismatch";
    const Binding current = CurrentBinding(slot, &failure);
    if (current.pawn != pawn)
    {
        BC_LOG_WARN("slot %d: registration failed (%s)", slot, failure);
        return false;
    }
    motion_recorder::InvalidateSlot(slot);
    {
        std::scoped_lock lock(g_bindingMutexes[slot]);
        g_bindings[slot] = current;
    }
    g_registeredSlots.fetch_or(uint64_t{ 1 } << slot, std::memory_order_release);
    return true;
}

// Clears Pawn registration; retains recordings.
void ClearReplayPawn(int slot)
{
    if (!ValidSlot(slot)) return;
    std::scoped_lock lock(g_bindingMutexes[slot]);
    g_bindings[slot] = {};
    g_registeredSlots.fetch_and(~(uint64_t{ 1 } << slot), std::memory_order_release);
}

// Resolves the registered Pawn.
void* ResolveReplayPawn(int slot, void* services)
{
    const Binding binding = RegisteredBinding(slot);
    return services && binding.services == services ? binding.pawn : nullptr;
}

namespace pawn_binding {
// Caches controller and Pawn offsets.
bool Init()
{
    g_server = static_cast<ISource2Server*>(g_SMAPI->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr));
    g_controllerPawn = cs2_sdk::schema::GetFieldOffset("CCSPlayerController", "m_hPlayerPawn");
    g_lifeState = cs2_sdk::schema::GetFieldOffset("CBaseEntity", "m_lifeState");
    g_controllerTickBase = cs2_sdk::schema::GetFieldOffset("CBasePlayerController", "m_nTickBase");
    return g_server && g_controllerPawn >= 0 && g_lifeState >= 0 && g_controllerTickBase >= 0;
}

// Returns only the registered, live services.
void* ServicesForSlot(int slot) { return RegisteredBinding(slot).services; }

// Reads live weapon services.
void* CurrentWeaponServices(int slot) { return ReadWeaponServices(CurrentBinding(slot)); }

// Reads registered weapon services.
void* RegisteredWeaponServices(int slot) { return ReadWeaponServices(RegisteredBinding(slot)); }

// New input requests explicitly bind the current pawn.
bool BindCurrentPawn(int slot)
{
    if (RegisteredBinding(slot).pawn) return true;
    return SetReplayPawn(slot, CurrentBinding(slot).pawn);
}

// Reads the service's owner slot.
int ServicesToSlot(void* services)
{
    void* pawn = nullptr;
    if (services) GuardedRead(services, tg::g_servicesPawn, pawn);
    return ControllerSlotForPawn(pawn);
}

// Resolves the registered callback's weapon services.
void* ServicesToWeaponServices(int slot, void* services)
{
    const Binding binding = RegisteredBinding(slot);
    return services && binding.services == services ? ReadWeaponServices(binding) : nullptr;
}

// Full commands advance tickbase before simulation.
bool CommandSimulationTick(int slot, void* services, const PlayerCommand& command, int& tick)
{
    const Binding binding = RegisteredBinding(slot);
    auto* globals = dispatch::g_engine ? dispatch::g_engine->GetServerGlobals() : nullptr;
    if (!services || binding.services != services || !globals ||
        !GuardedRead(binding.controller, g_controllerTickBase, tick) || tick < 0)
        return false;
    if (!command.parentcmd && globals->frametime != 0.0F)
    {
        if (tick == std::numeric_limits<int>::max()) return false;
        ++tick;
    }
    return true;
}

// Checks registered Pawn sessions.
void Refresh()
{
    uint64_t slots = g_registeredSlots.load(std::memory_order_acquire);
    while (slots)
    {
        RegisteredBinding(std::countr_zero(slots));
        slots &= slots - 1;
    }
}

// Ends sessions on map change or provider unload.
void Reset()
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
        motion_recorder::InvalidateSlot(slot);
}

// Clears registrations after callbacks drain.
void ClearAll()
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
        ClearReplayPawn(slot);
    g_server = nullptr;
    g_controllerPawn = -1;
    g_lifeState = -1;
    g_controllerTickBase = -1;
}
} // namespace pawn_binding
} // namespace cs2bc::input_injector
