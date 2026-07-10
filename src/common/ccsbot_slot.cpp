// CCSBot* -> player slot via pawn (+0x18) -> controller handle.

#include "ccsbot_slot.h"
#include "version_targets.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <tier0/dbg.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tg = BotController::targets;

namespace BotController
{
    // Compile-time switch to turn the once-per-bot diagnostic scan back on.
    static constexpr bool kEnableHandleScan = false;

    static std::unordered_set<void *> g_scanned;
    static std::mutex g_scannedMu;

    static int EntIndexFromHandle(uint32_t h)
    {
        if (h == 0u || h == 0xFFFFFFFFu)
            return -1;
        return static_cast<int>(h & 0x7FFFu);
    }

    static int SlotFromEntityIndex(int idx)
    {
        if (idx < 1 || idx > 64)
            return -1;
        return idx - 1;
    }

    // Reads an engine field and converts access violations into a failed lookup.
    bool TryReadMemory(const void *base, int offset, void *out, size_t size)
    {
        if (!base || !out || offset < 0 || size == 0)
            return false;

        const auto baseAddress = reinterpret_cast<uintptr_t>(base);
        const auto address = baseAddress + static_cast<uintptr_t>(offset);
        if (address < 0x10000u || address < baseAddress || address + size < address)
            return false;

#if defined(_WIN32)
        __try
        {
            std::memcpy(out, reinterpret_cast<const void *>(address), size);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        std::memcpy(out, reinterpret_cast<const void *>(address), size);
#endif
        return true;
    }

    // Writes an engine field and converts access violations into a failed update.
    bool TryWriteMemory(void *base, int offset, const void *value, size_t size)
    {
        if (!base || !value || offset < 0 || size == 0)
            return false;

        const auto baseAddress = reinterpret_cast<uintptr_t>(base);
        const auto address = baseAddress + static_cast<uintptr_t>(offset);
        if (address < 0x10000u || address < baseAddress || address + size < address)
            return false;

#if defined(_WIN32)
        __try
        {
            std::memcpy(reinterpret_cast<void *>(address), value, size);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        std::memcpy(reinterpret_cast<void *>(address), value, size);
#endif
        return true;
    }

    PawnControllerHandles ReadPawnControllerHandles(void *pawn)
    {
        PawnControllerHandles out{};
        out.controllerIndex = -1;
        out.originalControllerIndex = -1;
        out.controllerSlot = -1;
        out.ownerSlot = -1;

        if (!pawn)
            return out;

        if (!SafeRead(pawn, tg::kPawn_Controller, out.controllerHandle) ||
            !SafeRead(pawn, tg::kPawn_OriginalController, out.originalControllerHandle))
            return out;
        out.controllerIndex = EntIndexFromHandle(out.controllerHandle);
        out.originalControllerIndex = EntIndexFromHandle(out.originalControllerHandle);
        out.controllerSlot = SlotFromEntityIndex(out.controllerIndex);
        out.ownerSlot = out.controllerSlot >= 0
                            ? out.controllerSlot
                            : SlotFromEntityIndex(out.originalControllerIndex);
        return out;
    }

    static void ScanPawnForControllerHandle(void *pawn)
    {
        if (!pawn)
            return;
        Msg("[BWL][scan] pawn=%p candidate handles idx 1..64, 0x008..0x1000:\n", pawn);
        for (int off = 0x8; off < 0x1000; off += 4)
        {
            uint32_t v = 0;
            if (!SafeRead(pawn, off, v))
                continue;
            if (v == 0u || v == 0xFFFFFFFFu)
                continue;
            int idx = static_cast<int>(v & 0x7FFFu);
            uint32_t serial = (v >> 15);
            if (idx >= 1 && idx <= 64)
                Msg("[BWL][scan]   +0x%03X = 0x%08X  idx=%d  serial=%u\n",
                    off, v, idx, serial);
        }
    }

    SlotResolution ResolveSlot(void *bot)
    {
        SlotResolution out{nullptr, -1, -1};
        if (!bot)
            return out;

        void *pawn = nullptr;
        if (!SafeRead(bot, tg::kBot_Pawn, pawn))
            return out;
        if (!pawn)
            return out;
        out.pawn = pawn;

        void *identity = nullptr;
        if (!SafeRead(pawn, tg::kEnt_Identity, identity))
            return out;
        if (!identity)
            return out;

        uint32_t handle = 0;
        if (!SafeRead(identity, tg::kEntIdentity_EHandle, handle))
            return out;
        out.pawnEntIndex = EntIndexFromHandle(handle);
        if (out.pawnEntIndex <= 0)
            return out;

        out.slot = ReadPawnControllerHandles(pawn).ownerSlot;

        if (kEnableHandleScan)
        {
            std::lock_guard<std::mutex> lk(g_scannedMu);
            if (g_scanned.insert(bot).second)
                ScanPawnForControllerHandle(pawn);
        }

        return out;
    }

    int CCSBotToSlot(void *bot)
    {
        return ResolveSlot(bot).slot;
    }

    // Resolves direct bot pointers first, then the July 2026 helper context layout.
    SlotResolution ResolveSlotFromBotOrContext(void *botOrContext)
    {
        SlotResolution direct = ResolveSlot(botOrContext);
        if (direct.slot >= 0)
            return direct;

        void *bot = nullptr;
        if (!SafeRead(botOrContext, 0x10, bot))
            return direct;

        SlotResolution viaContext = ResolveSlot(bot);
        return viaContext.slot >= 0 ? viaContext : direct;
    }

    // Returns the slot resolved from either supported bot argument shape.
    int CCSBotContextToSlot(void *botOrContext)
    {
        return ResolveSlotFromBotOrContext(botOrContext).slot;
    }

    // Resolves the current controller and falls back to the stable original owner.
    int ControllerSlotForPawn(void *pawn)
    {
        if (!pawn)
            return -1;
        return ReadPawnControllerHandles(pawn).ownerSlot;
    }

    // CCSPlayerController*'s own identity ehandle -> entindex -> slot.
    int ControllerToSlot(void *controller)
    {
        if (!controller)
            return -1;
        void *identity = nullptr;
        if (!SafeRead(controller, tg::kEnt_Identity, identity))
            return -1;
        if (!identity)
            return -1;
        uint32_t h = 0;
        if (!SafeRead(identity, tg::kEntIdentity_EHandle, h))
            return -1;
        int idx = EntIndexFromHandle(h);
        if (idx < 1 || idx > 64)
            return -1;
        return idx - 1;
    }
}
