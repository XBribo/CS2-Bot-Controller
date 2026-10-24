// Internal movement-services ownership helpers; public registration API stays in InputInjector.h.
#pragma once

namespace cs2bc::input_injector::pawn_binding {
// Resolves registration dependencies.
bool Init();
// Returns validated services for the current registration.
void* ServicesForSlot(int slot);
// Binds the current pawn for a new input request.
bool BindCurrentPawn(int slot);
// Invalidates changed or disconnected pawns.
void Refresh();
// Ends all pawn sessions without discarding saved data.
void Reset();
// Reads the callback owner; registration is checked separately.
int ServicesToSlot(void* services, void** validatedPawn = nullptr);
// Resolves weapons only for the registered pawn.
void* ServicesToWeaponServices(int slot, void* services, void* validatedPawn);
// Clears all registered pawns during hook teardown.
void ClearAll();
} // namespace cs2bc::input_injector::pawn_binding
