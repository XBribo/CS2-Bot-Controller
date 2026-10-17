#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace cs2bc {
struct PawnControllerHandles
{
    uint32_t controllerHandle;
    uint32_t originalControllerHandle;
    int controllerIndex;
    int originalControllerIndex;
    int controllerSlot;
    int ownerSlot;
};

// Diagnostic: pawn pointer + pawn entindex used in slot computation.
struct SlotResolution
{
    void* pawn;
    int pawnEntIndex;
    int slot; // -1 on failure
};

// Returns slot in [0, 63] on success, or -1 if the pointer doesn't look
// like a CCSBot.
int CCSBotToSlot(void* bot);

// Full diagnostic version returning intermediate values.
SlotResolution ResolveSlot(void* bot);

// Resolves either a CCSBot pointer or a helper context containing one at +0x10.
SlotResolution ResolveSlotFromBotOrContext(void* botOrContext);

// Returns a player slot from a CCSBot pointer or helper context.
int CCSBotContextToSlot(void* botOrContext);

// Windows uses SEH; Linux requires trusted, still-live memory and copies directly.
bool TryReadMemory(const void* base, int offset, void* out, size_t size);

// Windows uses SEH; Linux requires trusted, writable memory and copies directly.
bool TryWriteMemory(void* base, int offset, const void* value, size_t size);

// Uses fault-protected reads on both platforms; failure may partially fill out.
bool TryReadMemoryGuarded(const void* base, int offset, void* out, size_t size);

// Uses fault-protected writes on both platforms; failure may leave a partial write.
bool TryWriteMemoryGuarded(void* base, int offset, const void* value, size_t size);

// Reads a field into a temporary and publishes it only after full success
template <typename T> bool SafeRead(const void* base, int offset, T& out)
{
    static_assert(std::is_trivially_copyable_v<T>);
    alignas(T) std::byte value[sizeof(T)]{};
    if (!TryReadMemory(base, offset, value, sizeof(T))) return false;
    std::memcpy(static_cast<void*>(&out), static_cast<const void*>(value), sizeof(T));
    return true;
}

// Reads an untrusted field and publishes it only after full success
template <typename T> bool GuardedRead(const void* base, int offset, T& out)
{
    static_assert(std::is_trivially_copyable_v<T>);
    alignas(T) std::byte value[sizeof(T)]{};
    if (!TryReadMemoryGuarded(base, offset, value, sizeof(T))) return false;
    std::memcpy(static_cast<void*>(&out), static_cast<const void*>(value), sizeof(T));
    return true;
}

// Uses TryWriteMemory's trusted Linux fast path, not TryWriteMemoryGuarded.
template <typename T> bool WriteField(void* base, int offset, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    return TryWriteMemory(base, offset, &value, sizeof(T));
}

// Resolves the owning slot through m_hController, then m_hOriginalController.
int ControllerSlotForPawn(void* pawn);

// Keeps full handles for identity checks; ownerSlot falls back to the original controller.
PawnControllerHandles ReadPawnControllerHandles(void* pawn);

// CCSPlayerController* (PhysicsSimulate arg0) -> slot via its own ehandle.
int ControllerToSlot(void* controller);
} // namespace cs2bc
