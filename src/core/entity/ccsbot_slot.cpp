#include "ccsbot_slot.h"
#include "core/cs2_sdk/src/utils/memory.h"
#include "offsets.h"
#include <entityhandle.h>

#include <cstdint>
#include <cstring>

namespace tg = cs2bc::offsets;

namespace cs2bc {

namespace {
// Player handles must be nonzero.
int EntIndexFromHandle(uint32_t h)
{
    if (h == 0U || h == 0xFFFFFFFFU) return -1;
    return CEntityHandle(h).GetEntryIndex();
}

// Only controller indices 1..64 map to player slots.
int SlotFromEntityIndex(int idx)
{
    if (idx < 1 || idx > 64) return -1;
    return idx - 1;
}

// Rejects low addresses and overflow.
uintptr_t FieldAddress(const void* base, int offset, size_t size)
{
    if (!base || offset < 0 || size == 0) return 0;
    const auto baseAddress = reinterpret_cast<uintptr_t>(base);
    const auto address = baseAddress + static_cast<uintptr_t>(offset);
    if (address < 0x10000U || address < baseAddress || address + size < address) return 0;
    return address;
}
} // namespace

bool TryReadMemory(const void* base, int offset, void* out, size_t size)
{
    const auto address = FieldAddress(base, offset, size);
    if (!address || !out) return false;

#ifdef _WIN32
    return cs2_sdk::memory::ReadBytes(reinterpret_cast<const void*>(address), out, size);
#else
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
#endif
}

bool TryWriteMemory(void* base, int offset, const void* value, size_t size)
{
    const auto address = FieldAddress(base, offset, size);
    if (!address || !value) return false;

#ifdef _WIN32
    return cs2_sdk::memory::WriteBytes(reinterpret_cast<void*>(address), value, size);
#else
    std::memcpy(reinterpret_cast<void*>(address), value, size);
    return true;
#endif
}

bool TryReadMemoryGuarded(const void* base, int offset, void* out, size_t size)
{
    const auto address = FieldAddress(base, offset, size);
    return address && out && cs2_sdk::memory::ReadBytes(reinterpret_cast<const void*>(address), out, size);
}

bool TryWriteMemoryGuarded(void* base, int offset, const void* value, size_t size)
{
    const auto address = FieldAddress(base, offset, size);
    return address && value && cs2_sdk::memory::WriteBytes(reinterpret_cast<void*>(address), value, size);
}

PawnControllerHandles ReadPawnControllerHandles(void* pawn)
{
    PawnControllerHandles out{};
    out.controllerIndex = -1;
    out.originalControllerIndex = -1;
    out.controllerSlot = -1;
    out.ownerSlot = -1;

    if (!pawn) return out;

    if (!GuardedRead(pawn, tg::g_pawnController, out.controllerHandle) ||
        !GuardedRead(pawn, tg::g_pawnOriginalController, out.originalControllerHandle))
        return out;
    out.controllerIndex = EntIndexFromHandle(out.controllerHandle);
    out.originalControllerIndex = EntIndexFromHandle(out.originalControllerHandle);
    out.controllerSlot = SlotFromEntityIndex(out.controllerIndex);
    out.ownerSlot = out.controllerSlot >= 0 ? out.controllerSlot : SlotFromEntityIndex(out.originalControllerIndex);
    return out;
}

SlotResolution ResolveSlot(void* bot)
{
    SlotResolution out{ .pawn = nullptr, .pawnEntIndex = -1, .slot = -1 };
    if (!bot) return out;

    void* pawn = nullptr;
    if (!GuardedRead(bot, tg::g_botPawn, pawn)) return out;
    if (!pawn) return out;
    out.pawn = pawn;

    void* identity = nullptr;
    if (!GuardedRead(pawn, tg::g_entIdentity, identity)) return out;
    if (!identity) return out;

    uint32_t handle = 0;
    if (!GuardedRead(identity, tg::g_entIdentityEHandle, handle)) return out;
    out.pawnEntIndex = EntIndexFromHandle(handle);
    if (out.pawnEntIndex <= 0) return out;

    out.slot = ReadPawnControllerHandles(pawn).ownerSlot;
    return out;
}

int CCSBotToSlot(void* bot) { return ResolveSlot(bot).slot; }

SlotResolution ResolveSlotFromBotOrContext(void* botOrContext)
{
    SlotResolution direct = ResolveSlot(botOrContext);
    if (direct.slot >= 0) return direct;

    void* bot = nullptr;
    if (!GuardedRead(botOrContext, 0x10, bot)) return direct;

    SlotResolution viaContext = ResolveSlot(bot);
    return viaContext.slot >= 0 ? viaContext : direct;
}

int CCSBotContextToSlot(void* botOrContext) { return ResolveSlotFromBotOrContext(botOrContext).slot; }

int ControllerSlotForPawn(void* pawn)
{
    if (!pawn) return -1;
    return ReadPawnControllerHandles(pawn).ownerSlot;
}

int ControllerToSlot(void* controller)
{
    if (!controller) return -1;
    void* identity = nullptr;
    if (!GuardedRead(controller, tg::g_entIdentity, identity)) return -1;
    if (!identity) return -1;
    uint32_t h = 0;
    if (!GuardedRead(identity, tg::g_entIdentityEHandle, h)) return -1;
    return SlotFromEntityIndex(EntIndexFromHandle(h));
}
} // namespace cs2bc
