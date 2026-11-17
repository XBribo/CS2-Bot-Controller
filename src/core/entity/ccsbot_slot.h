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

// Pawn and entity index used for slot resolution.
struct SlotResolution
{
    void* pawn;
    int pawnEntIndex;
    int slot; // -1 on failure
};

// Bot slot in [0, 63], or -1 on failure.
int CCSBotToSlot(void* bot);

// Resolves the slot and diagnostic fields.
SlotResolution ResolveSlot(void* bot);

// Resolves a Bot pointer or context holding one at +0x10.
SlotResolution ResolveSlotFromBotOrContext(void* botOrContext);

// Resolves the slot from a Bot or helper context.
int CCSBotContextToSlot(void* botOrContext);

// Windows: SEH. Linux: trusted live memory.
bool TryReadMemory(const void* base, int offset, void* out, size_t size);

// Windows: SEH. Linux: trusted writable memory.
bool TryWriteMemory(void* base, int offset, const void* value, size_t size);

// Guarded read; failure may partially fill out.
bool TryReadMemoryGuarded(const void* base, int offset, void* out, size_t size);

// Guarded write; failure may leave partial data.
bool TryWriteMemoryGuarded(void* base, int offset, const void* value, size_t size);

// Publishes a trusted field read on success.
template <typename T> bool SafeRead(const void* base, int offset, T& out)
{
    static_assert(std::is_trivially_copyable_v<T>);
    alignas(T) std::byte value[sizeof(T)]{};
    if (!TryReadMemory(base, offset, value, sizeof(T))) return false;
    std::memcpy(static_cast<void*>(&out), static_cast<const void*>(value), sizeof(T));
    return true;
}

// Publishes a guarded field read on success.
template <typename T> bool GuardedRead(const void* base, int offset, T& out)
{
    static_assert(std::is_trivially_copyable_v<T>);
    alignas(T) std::byte value[sizeof(T)]{};
    if (!TryReadMemoryGuarded(base, offset, value, sizeof(T))) return false;
    std::memcpy(static_cast<void*>(&out), static_cast<const void*>(value), sizeof(T));
    return true;
}

// Uses the trusted Linux write path.
template <typename T> bool WriteField(void* base, int offset, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    return TryWriteMemory(base, offset, &value, sizeof(T));
}

// Resolves the owning slot through m_hController, then m_hOriginalController.
int ControllerSlotForPawn(void* pawn);

// Reads identity handles; falls back to the original controller.
PawnControllerHandles ReadPawnControllerHandles(void* pawn);

// Controller entity handle to player slot.
int ControllerToSlot(void* controller);
} // namespace cs2bc
