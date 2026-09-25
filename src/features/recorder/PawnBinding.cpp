#include "PawnBinding.h"
#include "InputInjector.h"
#include "MotionRecorder.h"
#include "ccsbot_slot.h"
#include "offsets.h"

#include <array>
#include <atomic>

namespace tg = cs2bc::offsets;

namespace cs2bc::input_injector {
namespace {
// Registered pawns belong to this module, not the hook scheduler.
std::array<std::atomic<void*>, kMaxSlots> g_slotPawns{};

// Checks the fixed registration table's slot bounds.
bool ValidSlotIndex(int slot) { return slot >= 0 && slot < kMaxSlots; }

// Reads the helper pawn field embedded in movement services.
void* ServicesToPawnField(void* services)
{
    void* pawn = nullptr;
    return GuardedRead(services, tg::g_servicesPawn, pawn) ? pawn : nullptr;
}

// Verifies that a pawn currently owns the supplied movement services.
bool PawnOwnsServices(void* pawn, void* services)
{
    if (!pawn || !services) return false;
    void* liveServices = nullptr;
    return GuardedRead(pawn, tg::g_pawnMovementServices, liveServices) && liveServices == services;
}

} // namespace

// Registers a readable pawn whose current owner matches the requested slot.
bool SetReplayPawn(int slot, void* pawn)
{
    if (!ValidSlotIndex(slot) || motion_recorder::IsReplaying(slot)) return false;
    g_slotPawns[slot].store(nullptr, std::memory_order_release);
    if (!pawn) return false;

    void* identity = nullptr;
    uint32_t handle = 0;
    if (!GuardedRead(pawn, tg::g_entIdentity, identity) || !identity || !GuardedRead(identity, tg::g_entIdentityEHandle, handle) ||
        handle == 0U || handle == 0xFFFFFFFFU)
        return false;

    int ownerSlot = ControllerSlotForPawn(pawn);
    if (ownerSlot >= 0 && ownerSlot != slot) return false;

    void* services = nullptr;
    if (!GuardedRead(pawn, tg::g_pawnMovementServices, services) || !services || ServicesToPawnField(services) != pawn) return false;

    g_slotPawns[slot].store(pawn, std::memory_order_release);
    PrimeSlotServices(slot, services);
    return true;
}

// Removes a registered pawn before the entity can be recycled.
void ClearReplayPawn(int slot)
{
    if (ValidSlotIndex(slot)) g_slotPawns[slot].store(nullptr, std::memory_order_release);
}

// Returns a registered pawn only when its movement-services link is current.
void* ResolveReplayPawn(int slot, void* services)
{
    if (ValidSlotIndex(slot))
    {
        void* registered = g_slotPawns[slot].load(std::memory_order_acquire);
        if (PawnOwnsServices(registered, services)) return registered;
    }

    void* fieldPawn = ServicesToPawnField(services);
    return PawnOwnsServices(fieldPawn, services) ? fieldPawn : nullptr;
}

namespace {

// Finds a registered slot by validating every pawn-to-services link.
int RegisteredSlotForServices(void* services)
{
    if (!services) return -1;
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        void* pawn = g_slotPawns[slot].load(std::memory_order_acquire);
        if (PawnOwnsServices(pawn, services)) return slot;
    }
    return -1;
}

} // namespace

namespace pawn_binding {
// Optionally returns the field pawn validated during this same resolution.
int ServicesToSlot(void* services, void** validatedPawn)
{
    if (validatedPawn) *validatedPawn = nullptr;
    if (!services) return -1;
    void* pawn = ServicesToPawnField(services);
    if (!PawnOwnsServices(pawn, services)) pawn = nullptr;
    if (validatedPawn) *validatedPawn = pawn;

    PawnControllerHandles handles = ReadPawnControllerHandles(pawn);
    int ownerSlot = handles.ownerSlot;
    if (ownerSlot < 0) ownerSlot = RegisteredSlotForServices(services);
    return ownerSlot;
}

// Reuses this callback's field pawn while retaining registered-pawn precedence.
void* ServicesToWeaponServices(int slot, void* services, void* validatedPawn)
{
    void* registered = ValidSlotIndex(slot) ? g_slotPawns[slot].load(std::memory_order_acquire) : nullptr;
    void* pawn = validatedPawn;
    if (!pawn || (registered && registered != pawn)) pawn = ResolveReplayPawn(slot, services);
    if (!pawn) return nullptr;
    void* weaponServices = nullptr;
    return GuardedRead(pawn, tg::g_pawnWeaponServices, weaponServices) ? weaponServices : nullptr;
}

// Clears registrations when input hooks are removed.
void ClearAll()
{
    for (auto& pawn : g_slotPawns)
        pawn.store(nullptr, std::memory_order_release);
}
} // namespace pawn_binding
} // namespace cs2bc::input_injector
