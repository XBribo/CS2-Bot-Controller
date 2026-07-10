// Convert a CCSBot* (server-side entity) to its player slot (0..63).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace BotController
{
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
        void *pawn;
        int pawnEntIndex;
        int slot; // -1 on failure
    };

    // Returns slot in [0, 63] on success, or -1 if the pointer doesn't look
    // like a CCSBot.
    int CCSBotToSlot(void *bot);

    // Full diagnostic version returning intermediate values.
    SlotResolution ResolveSlot(void *bot);

    // Resolves either a CCSBot pointer or a helper context containing one at +0x10.
    SlotResolution ResolveSlotFromBotOrContext(void *botOrContext);

    // Returns a player slot from a CCSBot pointer or helper context.
    int CCSBotContextToSlot(void *botOrContext);

    // Reads engine memory without allowing an invalid pointer to crash Windows.
    bool TryReadMemory(const void *base, int offset, void *out, size_t size);

    // Writes engine memory without allowing an invalid pointer to crash Windows.
    bool TryWriteMemory(void *base, int offset, const void *value, size_t size);

    // Reads a field into a temporary and publishes it only after full success
    template <typename T>
    bool SafeRead(const void *base, int offset, T &out)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        alignas(T) std::byte value[sizeof(T)]{};
        if (!TryReadMemory(base, offset, value, sizeof(T)))
            return false;
        std::memcpy(&out, value, sizeof(T));
        return true;
    }

    // Writes a trivially-copyable engine field through the guarded memory path.
    template <typename T>
    bool WriteField(void *base, int offset, const T &value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return TryWriteMemory(base, offset, &value, sizeof(T));
    }

    // Resolves the owning slot through m_hController, then m_hOriginalController.
    int ControllerSlotForPawn(void *pawn);

    PawnControllerHandles ReadPawnControllerHandles(void *pawn);

    // CCSPlayerController* (PhysicsSimulate arg0) -> slot via its own ehandle.
    int ControllerToSlot(void *controller);
}
