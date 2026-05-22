// Convert a CCSBot* (server-side entity) to its player slot (0..63).

#pragma once

namespace BotWeaponLock
{
    // Diagnostic: pawn pointer + pawn entindex used in slot computation.
    struct SlotResolution
    {
        void *pawn;
        int   pawnEntIndex;
        int   slot;          // -1 on failure
    };

    // Returns slot in [0, 63] on success, or -1 if the pointer doesn't look
    // like a CCSBot.
    int CCSBotToSlot(void *bot);

    // Full diagnostic version returning intermediate values.
    SlotResolution ResolveSlot(void *bot);
}
