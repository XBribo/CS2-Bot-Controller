// Shared Pawn, service, and weapon resolution.
#pragma once

class PlayerCommand;

namespace cs2bc::input_injector::pawn_binding {
// Initializes shared pawn and weapon resolution.
bool Init();
// Resolves registered movement services.
void* ServicesForSlot(int slot);
// Reads current live weapon services.
void* CurrentWeaponServices(int slot);
// Reads registered weapon services.
void* RegisteredWeaponServices(int slot);
// Binds the current pawn for a new input request.
bool BindCurrentPawn(int slot);
// Invalidates changed or disconnected pawns.
void Refresh();
// Ends Pawn sessions and retains recordings.
void Reset();
// Reads the callback's owner slot.
int ServicesToSlot(void* services);
// Resolves weapons only for the registered pawn.
void* ServicesToWeaponServices(int slot, void* services);
// Reads the command's simulation tick.
bool CommandSimulationTick(int slot, void* services, const PlayerCommand& command, int& tick);
// Clears registrations after hooks drain.
void ClearAll();
} // namespace cs2bc::input_injector::pawn_binding
